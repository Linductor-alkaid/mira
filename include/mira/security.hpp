#pragma once

#include <mira/artifact_store.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

using CapabilityId = std::string;
using GrantVersion = std::uint64_t;
using PolicyVersion = std::uint64_t;

enum class AuthenticationStrength : std::uint8_t { Anonymous, Session, Strong };
enum class GrantSource : std::uint8_t { Host, Administrator, System };
enum class ActionRisk : std::uint8_t { R0ReadOnly, R1ReversibleLow, R2UserVisible, R3Sensitive, R4Critical };
enum class ConfirmationDecision : std::uint8_t { Approve, Reject };

struct ResourceSelector final {
    std::string type;
    std::string scope;
    std::string pattern;
};

struct CapabilityGrant final {
    CapabilityId capability;
    ResourceSelector resources;
    GrantSource source = GrantSource::Host;
    GrantVersion version = 0;
    std::chrono::system_clock::time_point expires_at{};
};

struct PrincipalContext final {
    TenantId tenant_id;
    UserId user_id;
    HostInstanceId host_id;
    AuthenticationStrength auth_strength = AuthenticationStrength::Anonymous;
    std::chrono::system_clock::time_point authenticated_at{};
    std::vector<CapabilityGrant> grants;
    GrantVersion identity_version = 0;
};

struct RedactionRecord final {
    Sensitivity sensitivity = Sensitivity::Internal;
    std::size_t original_size = 0;
    Sha256Digest original_digest;
    bool redacted = false;
};

struct ProposedEffect final {
    std::string action;
    std::string parameters;
    ActionRisk risk = ActionRisk::R4Critical;
    bool has_side_effect = true;
};

struct ResourceDescriptor final {
    std::string type;
    std::string id;
    std::string scope;
};

struct PolicyInput final {
    PrincipalContext principal;
    ProposedEffect effect;
    ResourceDescriptor target;
    Sensitivity sensitivity = Sensitivity::Internal;
};

struct AllowDecision final {
    PolicyVersion policy_version = 1;
    std::string rule_id;
    std::string safe_reason;
};
struct DenyDecision final {
    PolicyVersion policy_version = 1;
    std::string rule_id;
    std::string safe_reason;
};
struct RequireConfirmationDecision final {
    PolicyVersion policy_version = 1;
    std::string rule_id;
    std::string safe_reason;
};
using PolicyDecision = std::variant<AllowDecision, DenyDecision, RequireConfirmationDecision>;

class PolicyEngine final {
public:
    explicit PolicyEngine(PolicyVersion version = 1) : version_(version) {}
    [[nodiscard]] PolicyDecision evaluate(const PolicyInput &) const;

private:
    PolicyVersion version_;
};

struct ConfirmationChallenge final {
    ConfirmationId id;
    PrincipalContext principal;
    SessionId session_id;
    TaskId task_id;
    std::uint64_t task_epoch = 0;
    std::uint64_t environment_epoch = 0;
    Sha256Digest action_digest;
    ResourceDescriptor target;
    ActionRisk risk = ActionRisk::R4Critical;
    std::chrono::system_clock::time_point expires_at{};
    Id128 nonce;
    PolicyVersion policy_version = 1;
};

struct ConfirmationResponse final {
    ConfirmationId challenge_id;
    Id128 nonce;
    UserId user_id;
    ConfirmationDecision decision = ConfirmationDecision::Reject;
    std::string auth_evidence;
};

class ConfirmationAuthority final {
public:
    [[nodiscard]] Result<ConfirmationChallenge> issue(
        const PrincipalContext &, SessionId, TaskId, std::uint64_t task_epoch,
        std::uint64_t environment_epoch, const ProposedEffect &, const ResourceDescriptor &,
        PolicyVersion, std::chrono::seconds lifetime = std::chrono::seconds(60));
    [[nodiscard]] Result<void> consume(const ConfirmationChallenge &, const ConfirmationResponse &,
                                       const PrincipalContext &, const ProposedEffect &,
                                       const ResourceDescriptor &, std::uint64_t task_epoch,
                                       std::uint64_t environment_epoch, PolicyVersion);
    [[nodiscard]] bool is_consumed(ConfirmationId) const;

private:
    struct State final {
        ConfirmationChallenge challenge;
        bool consumed = false;
    };
    mutable std::mutex mutex_;
    std::vector<State> states_;
};

class Redactor final {
public:
    [[nodiscard]] static std::string redact(std::string_view value, Sensitivity sensitivity);
    [[nodiscard]] static RedactionRecord record(std::string_view value, Sensitivity sensitivity);
};

struct EndpointPolicy final {
    std::vector<std::string> allowed_hosts;
    bool allow_private = false;
};

[[nodiscard]] bool endpoint_allowed(std::string_view url, const EndpointPolicy &policy);
[[nodiscard]] bool path_within_root(const std::filesystem::path &root,
                                    const std::filesystem::path &candidate);
[[nodiscard]] bool contains_prompt_injection(std::string_view value);

} // namespace mira
