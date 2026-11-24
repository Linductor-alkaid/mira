#pragma once

#include <mira/core_contracts.hpp>

namespace mira {

struct SocialEnvironmentKey final {
    TenantId tenant;
    Id128 environment;
    friend constexpr bool operator==(const SocialEnvironmentKey &,
                                     const SocialEnvironmentKey &) = default;
};

struct SocialObjectKey final {
    SocialEnvironmentKey environment;
    Id128 object; // Internal ID allocated by the host within this environment.
    friend constexpr bool operator==(const SocialObjectKey &, const SocialObjectKey &) = default;
};

struct SocialAccountKey final {
    std::string source; // Stable connector namespace from authenticated message metadata.
    Id128 account;      // Host-issued opaque account identity within that namespace.
    friend bool operator==(const SocialAccountKey &, const SocialAccountKey &) = default;
};

struct SocialRelationshipKey final {
    SocialObjectKey from;
    SocialObjectKey to;
    friend constexpr bool operator==(const SocialRelationshipKey &,
                                     const SocialRelationshipKey &) = default;
};

} // namespace mira
