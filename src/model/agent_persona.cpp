#include <mira/agent_persona.hpp>

namespace mira {
namespace {

Error persona_error(ErrorCode code, const char *message) {
    Error error;
    error.code = code;
    error.domain = "mira.persona";
    error.safe_message = message;
    return error;
}

} // namespace

Result<void> AgentPersonaCatalog::activate(const AgentPersonaKey &key, AgentPersonaRevision next) {
    constexpr std::size_t kMaxInstructionBytes = 8192;
    if (key.tenant.is_nil() || key.agent.is_nil() || next.version == 0 ||
        next.instructions.size() > kMaxInstructionBytes ||
        next.instructions.find_first_not_of(" \t\r\n\f\v") == std::string::npos ||
        next.instructions.find('\0') != std::string::npos)
        return persona_error(ErrorCode::InvalidArgument, "invalid persona revision");

    const std::lock_guard lock(mutex_);
    const auto found = active_.find(key);
    if (found != active_.end() && found->second.revision->version == next.version) {
        if (found->second.revision->instructions == next.instructions)
            return {};
        return persona_error(ErrorCode::AlreadyExists, "active persona revision has other content");
    }
    if (found == active_.end() && active_.size() >= max_agents_)
        return persona_error(ErrorCode::ResourceExhausted, "persona catalog is at capacity");

    active_.insert_or_assign(key,
                             Entry{std::make_shared<const AgentPersonaRevision>(std::move(next)),
                                   std::make_shared<const bool>(true)});
    return {};
}

Result<AgentPersonaSnapshot> AgentPersonaCatalog::read(const AgentPersonaKey &key) const {
    const std::lock_guard lock(mutex_);
    const auto found = active_.find(key);
    if (found == active_.end())
        return persona_error(ErrorCode::NotFound, "persona has no active revision");
    return AgentPersonaSnapshot{key, found->second.revision, found->second.activation};
}

void AgentPersonaCatalog::deactivate(const AgentPersonaKey &key) {
    const std::lock_guard lock(mutex_);
    active_.erase(key);
}

} // namespace mira
