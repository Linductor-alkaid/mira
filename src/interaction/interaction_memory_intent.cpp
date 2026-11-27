#include "interaction_coordinator_support.hpp"

#include <algorithm>
#include <array>

namespace mira {
namespace {
enum class Intent { Remember, Correct, Forget };

struct Command final {
    Intent intent;
    std::string_view key;
    std::string_view statement;
};

struct CommandPhrase final {
    Intent intent;
    std::string_view prefix;
};

constexpr std::array phrases{
    CommandPhrase{Intent::Remember, "记住偏好"},
    CommandPhrase{Intent::Correct, "纠正偏好"},
    CommandPhrase{Intent::Forget, "忘掉偏好"},
    CommandPhrase{Intent::Remember, "remember preference"},
    CommandPhrase{Intent::Correct, "correct preference"},
    CommandPhrase{Intent::Forget, "forget preference"},
};

Error intent_error(ErrorCode code, const char *message) {
    return interaction_error(code, InteractionDomainCode::InputInvalid, message);
}

std::string_view trim_spaces(std::string_view text) {
    if (const auto first = text.find_first_not_of(' '); first != std::string_view::npos)
        return text.substr(first, text.find_last_not_of(' ') - first + 1);
    return {};
}

bool key_character(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-';
}

Result<std::optional<Command>> parse_command(std::string_view input) {
    auto text = trim_spaces(input);
    if (text.starts_with("请"))
        text = trim_spaces(text.substr(std::string_view{"请"}.size()));
    else if (memory_normalize_claim_key(text.substr(0, 7)) == "please" && text.size() >= 7 &&
             text[6] == ' ')
        text = trim_spaces(text.substr(7));

    for (const auto &phrase : phrases) {
        if (text.size() < phrase.prefix.size() ||
            memory_normalize_claim_key(text.substr(0, phrase.prefix.size())) != phrase.prefix)
            continue;
        auto argument = text.substr(phrase.prefix.size());
        if (!argument.empty() && !argument.starts_with(' ') && !argument.starts_with(':') &&
            !argument.starts_with("："))
            continue;
        argument = trim_spaces(argument);
        if (argument.starts_with(':'))
            argument = trim_spaces(argument.substr(1));
        else if (argument.starts_with("："))
            argument = trim_spaces(argument.substr(std::string_view{"："}.size()));

        const auto assignment = argument.find('=');
        const auto key = trim_spaces(argument.substr(0, assignment));
        if (key.empty() || !std::ranges::all_of(key, key_character) ||
            std::ranges::any_of(argument, [](char character) {
                return static_cast<unsigned char>(character) < ' ' || character == '\x7f';
            }))
            return intent_error(ErrorCode::InvalidArgument,
                                "memory command requires a single line and a preference key");
        if (phrase.intent == Intent::Forget) {
            if (assignment != std::string_view::npos)
                return intent_error(ErrorCode::InvalidArgument,
                                    "forget preference requires one key");
        } else if (assignment == std::string_view::npos ||
                   trim_spaces(argument.substr(assignment + 1)).empty()) {
            return intent_error(ErrorCode::InvalidArgument,
                                "remember or correct preference requires key=value");
        }
        if (!parse_json(to_json_string(JsonValue{std::string(argument)})))
            return intent_error(ErrorCode::InvalidArgument, "memory command is not valid text");
        return std::optional{Command{phrase.intent, key, argument}};
    }
    return std::optional<Command>{};
}

Id128 prepared_id(ControlIngressId source) {
    const auto digest = digest_string("mira.interaction.memory-input.v1/" + source.to_string());
    std::array<std::uint8_t, 16> bytes{};
    std::copy_n(digest.bytes.begin(), bytes.size(), bytes.begin());
    return Id128{bytes};
}
} // namespace

Result<std::optional<PreparedMemoryIntent>>
InteractionCoordinator::prepare_memory_intent(const PrincipalContext &principal,
                                              ControlIngressId ingress, std::string_view text,
                                              const OperationContext &operation) const try {
    if (ingress.is_nil() || text.size() > Impl::kMaxMemoryIntentInputBytes || !operation.deadline)
        return intent_error(ErrorCode::InvalidArgument, "memory intent source is invalid");
    if (operation.cancelled())
        return intent_error(ErrorCode::Cancelled, "memory intent preparation was cancelled");
    if (operation.expired(Timestamp::now()))
        return intent_error(ErrorCode::DeadlineExceeded, "memory intent preparation expired");

    std::shared_ptr<MemoryAgentService> service;
    {
        const std::lock_guard lock(impl_->submit_mutex);
        if (!impl_->initialized || impl_->stopping.load() || !impl_->memory)
            return intent_error(ErrorCode::InvalidState, "memory intent owner is unavailable");
        const auto found = impl_->memory_works.find(operation.task);
        if (found == impl_->memory_works.end() || found->second.status.suspended ||
            found->second.status.terminal ||
            !interaction_support::permitted(principal, impl_->config))
            return intent_error(ErrorCode::PermissionDenied, "memory input is unavailable");
        const auto &binding = found->second.binding;
        if (binding.principal.tenant_id != principal.tenant_id ||
            binding.principal.user_id != principal.user_id ||
            binding.principal.identity_version != principal.identity_version ||
            binding.work.session_id != operation.session ||
            binding.work.task_epoch != operation.task_epoch || binding.input_source != ingress ||
            found->second.input_digest != digest_string(text) ||
            (binding.request_current && !binding.request_current()))
            return intent_error(ErrorCode::PermissionDenied, "memory input is unavailable");
        service = impl_->memory;
    }

    const auto parsed = parse_command(text);
    if (!parsed)
        return parsed.error();
    if (!parsed.value())
        return std::optional<PreparedMemoryIntent>{};
    const auto &command = *parsed.value();
    const auto id = prepared_id(ingress);
    if (command.intent == Intent::Remember)
        return std::optional{PreparedMemoryIntent{
            PreferenceIntentRequest{PreferenceIntent::Remember,
                                    {MutationId{id}, std::string(command.statement), {}, {}},
                                    ingress}}};

    const auto target = service->resolve_memory_target(
        principal, {MemoryScopeKind::User, MemoryKind::Preference, std::string(command.key)},
        operation);
    if (!target)
        return target.error();
    if (!target.value())
        return intent_error(ErrorCode::NotFound, "no current preference matches the memory input");
    const auto &record = *target.value();
    if (command.intent == Intent::Forget)
        return std::optional{PreparedMemoryIntent{MemoryForgetRequest{
            OperationId{id}, MemoryScopeKind::User, record.record, record.version, ingress}}};
    return std::optional{PreparedMemoryIntent{PreferenceIntentRequest{
        PreferenceIntent::Correct,
        {MutationId{id}, std::string(command.statement), record.record, record.version},
        ingress}}};
} catch (...) {
    return intent_error(ErrorCode::Unavailable, "memory intent preparation failed");
}
} // namespace mira
