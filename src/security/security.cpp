#include <mira/security.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <sstream>

namespace mira {
namespace {

Error error(ErrorCode code, std::string message) {
    Error result;
    result.code = code;
    result.domain = "mira.security";
    result.safe_message = std::move(message);
    return result;
}

std::string effect_canonical(const ProposedEffect &effect, const ResourceDescriptor &target) {
    return effect.action + "|" + effect.parameters + "|" +
           std::to_string(static_cast<int>(effect.risk)) + "|" + target.type + "|" + target.id +
           "|" + target.scope;
}

bool has_capability(const PrincipalContext &principal, std::string_view capability,
                    const ResourceDescriptor &target) {
    const auto now = std::chrono::system_clock::now();
    return std::any_of(principal.grants.begin(), principal.grants.end(), [&](const auto &grant) {
        if (grant.capability != capability ||
            (grant.expires_at != std::chrono::system_clock::time_point{} &&
             grant.expires_at <= now)) {
            return false;
        }
        if (!grant.resources.type.empty() && grant.resources.type != target.type)
            return false;
        if (!grant.resources.scope.empty() && grant.resources.scope != target.scope)
            return false;
        return grant.resources.pattern.empty() || grant.resources.pattern == "*" ||
               grant.resources.pattern == target.id;
    });
}

} // namespace

PolicyDecision PolicyEngine::evaluate(const PolicyInput &input) const {
    const auto capability = input.effect.action;
    if (!has_capability(input.principal, capability, input.target)) {
        return DenyDecision{version_, "capability-missing", "required capability is not granted"};
    }
    if (input.effect.risk == ActionRisk::R4Critical) {
        return DenyDecision{version_, "critical-default-deny",
                            "critical actions require explicit host policy"};
    }
    if (input.effect.risk == ActionRisk::R3Sensitive ||
        input.effect.risk == ActionRisk::R2UserVisible ||
        input.sensitivity >= Sensitivity::Sensitive) {
        return RequireConfirmationDecision{version_, "confirmation-required",
                                           "user confirmation is required"};
    }
    if (input.effect.has_side_effect && input.effect.action.empty()) {
        return DenyDecision{version_, "invalid-action", "action capability is empty"};
    }
    return AllowDecision{version_, "grant", "capability and risk policy allow the effect"};
}

Result<ConfirmationChallenge> ConfirmationAuthority::issue(
    const PrincipalContext &principal, SessionId session_id, TaskId task_id,
    std::uint64_t task_epoch, std::uint64_t environment_epoch, const ProposedEffect &effect,
    const ResourceDescriptor &target, PolicyVersion policy_version, std::chrono::seconds lifetime) {
    if (principal.user_id.is_nil() || session_id.is_nil() || task_id.is_nil()) {
        return error(ErrorCode::InvalidArgument, "confirmation identity and scope must be non-nil");
    }
    ConfirmationChallenge challenge;
    challenge.id = ConfirmationId::generate();
    challenge.principal = principal;
    challenge.session_id = session_id;
    challenge.task_id = task_id;
    challenge.task_epoch = task_epoch;
    challenge.environment_epoch = environment_epoch;
    challenge.action_digest = digest_string(effect_canonical(effect, target));
    challenge.target = target;
    challenge.risk = effect.risk;
    challenge.expires_at = std::chrono::system_clock::now() + lifetime;
    challenge.nonce = Id128::generate();
    challenge.policy_version = policy_version;
    {
        std::lock_guard lock(mutex_);
        states_.push_back(State{challenge, false});
    }
    return challenge;
}

Result<void>
ConfirmationAuthority::consume(const ConfirmationChallenge &challenge,
                               const ConfirmationResponse &response,
                               const PrincipalContext &principal, const ProposedEffect &effect,
                               const ResourceDescriptor &target, std::uint64_t task_epoch,
                               std::uint64_t environment_epoch, PolicyVersion policy_version) {
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(states_.begin(), states_.end(), [&](const auto &state) {
        return state.challenge.id == challenge.id;
    });
    if (found == states_.end())
        return error(ErrorCode::NotFound, "confirmation challenge is unknown");
    if (found->consumed)
        return error(ErrorCode::AlreadyExists, "confirmation challenge was already consumed");
    const auto &stored = found->challenge;
    if (response.challenge_id != stored.id || response.nonce != stored.nonce ||
        response.user_id != principal.user_id || stored.principal.user_id != principal.user_id ||
        stored.principal.tenant_id != principal.tenant_id ||
        stored.principal.host_id != principal.host_id ||
        stored.session_id != challenge.session_id || stored.task_id != challenge.task_id) {
        return error(ErrorCode::PermissionDenied,
                     "confirmation response identity or nonce mismatch");
    }
    if (response.decision != ConfirmationDecision::Approve)
        return error(ErrorCode::SafetyRejected, "confirmation rejected");
    if (std::chrono::system_clock::now() >= stored.expires_at)
        return error(ErrorCode::DeadlineExceeded, "confirmation expired");
    if (task_epoch != stored.task_epoch || environment_epoch != stored.environment_epoch ||
        policy_version != stored.policy_version ||
        stored.action_digest != digest_string(effect_canonical(effect, target))) {
        return error(ErrorCode::SafetyRejected,
                     "confirmation binding no longer matches the requested effect");
    }
    found->consumed = true;
    return Result<void>{};
}

bool ConfirmationAuthority::is_consumed(ConfirmationId id) const {
    std::lock_guard lock(mutex_);
    const auto found = std::find_if(states_.begin(), states_.end(),
                                    [&](const auto &state) { return state.challenge.id == id; });
    return found != states_.end() && found->consumed;
}

std::string Redactor::redact(std::string_view value, Sensitivity sensitivity) {
    if (sensitivity == Sensitivity::Public || sensitivity == Sensitivity::Internal)
        return std::string(value);
    if (value.empty())
        return "<redacted>";
    return "<redacted:" + std::to_string(value.size()) + ">";
}

RedactionRecord Redactor::record(std::string_view value, Sensitivity sensitivity) {
    return RedactionRecord{sensitivity, value.size(), digest_string(value),
                           sensitivity == Sensitivity::Sensitive ||
                               sensitivity == Sensitivity::Secret};
}

namespace {
std::string lower_copy(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool private_ipv4(std::string_view host) {
    unsigned int octets[4]{};
    char dot = 0;
    std::istringstream input{std::string(host)};
    if (!(input >> octets[0] >> dot) || dot != '.')
        return false;
    if (!(input >> octets[1] >> dot) || dot != '.')
        return false;
    if (!(input >> octets[2] >> dot) || dot != '.')
        return false;
    if (!(input >> octets[3]))
        return false;
    if (octets[0] > 255 || octets[1] > 255 || octets[2] > 255 || octets[3] > 255)
        return false;
    return octets[0] == 10 || octets[0] == 127 || (octets[0] == 169 && octets[1] == 254) ||
           (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
           (octets[0] == 192 && octets[1] == 168) || octets[0] == 0;
}
} // namespace

bool endpoint_allowed(std::string_view url, const EndpointPolicy &policy) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos)
        return false;
    const auto scheme = lower_copy(url.substr(0, scheme_end));
    if (scheme != "https" && scheme != "http")
        return false;
    auto authority = url.substr(scheme_end + 3);
    const auto path_start = authority.find_first_of("/?#");
    if (path_start != std::string_view::npos)
        authority = authority.substr(0, path_start);
    if (authority.empty() || authority.find('@') != std::string_view::npos)
        return false;
    std::string host;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos)
            return false;
        host = lower_copy(authority.substr(1, close - 1));
        if (close + 1 < authority.size() && authority[close + 1] != ':')
            return false;
    } else {
        const auto colon = authority.find(':');
        host = lower_copy(authority.substr(0, colon));
        if (host.empty())
            return false;
        if (colon != std::string_view::npos) {
            const auto port = authority.substr(colon + 1);
            if (port.empty() || !std::all_of(port.begin(), port.end(),
                                             [](unsigned char c) { return std::isdigit(c); }))
                return false;
        }
    }
    if (!policy.allow_private && (host == "localhost" || host == "::1" || private_ipv4(host) ||
                                  host == "metadata.google.internal" || host == "169.254.169.254"))
        return false;
    return std::any_of(
        policy.allowed_hosts.begin(), policy.allowed_hosts.end(), [&](const auto &allowed) {
            const auto rule = lower_copy(allowed);
            if (rule == host)
                return true;
            if (rule.size() > 2 && rule.rfind("*.", 0) == 0) {
                const auto suffix = rule.substr(1);
                return host.size() > suffix.size() &&
                       host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
            }
            return false;
        });
}

bool path_within_root(const std::filesystem::path &root, const std::filesystem::path &candidate) {
    std::error_code ec;
    const auto canonical_root = std::filesystem::weakly_canonical(root, ec);
    if (ec)
        return false;
    const auto canonical_candidate = std::filesystem::weakly_canonical(candidate, ec);
    if (ec)
        return false;
    const auto relative = canonical_candidate.lexically_relative(canonical_root);
    if (relative.empty() || relative == "." || relative == ".." || relative.is_absolute())
        return false;
    const auto first = *relative.begin();
    return first != "..";
}

bool contains_prompt_injection(std::string_view value) {
    const auto normalized = lower_copy(value);
    static constexpr std::string_view markers[] = {"ignore previous instructions",
                                                   "ignore all instructions",
                                                   "system message",
                                                   "developer message",
                                                   "jailbreak",
                                                   "override the system",
                                                   "reveal the prompt",
                                                   "exfiltrate"};
    return std::any_of(std::begin(markers), std::end(markers),
                       [&](auto marker) { return normalized.find(marker) != std::string::npos; });
}

} // namespace mira
