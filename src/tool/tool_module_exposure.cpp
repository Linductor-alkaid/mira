#include <mira/tool_module_exposure.hpp>

#include <mira/model_tool.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace mira {

namespace {

Error make_exposure_error(ErrorCode code, std::string safe_message) {
    Error error;
    error.code = code;
    error.domain = "mira.tool_module";
    error.safe_message = std::move(safe_message);
    return error;
}

[[nodiscard]] JsonValue version_to_json(const SemanticVersion &version) {
    JsonValue::Object object;
    object.emplace_back("major", static_cast<std::int64_t>(version.major));
    object.emplace_back("minor", static_cast<std::int64_t>(version.minor));
    object.emplace_back("patch", static_cast<std::int64_t>(version.patch));
    return JsonValue{std::move(object)};
}

[[nodiscard]] std::string side_effect_name(ActionRisk risk) {
    switch (risk) {
    case ActionRisk::R0ReadOnly:
        return "read_only";
    case ActionRisk::R1ReversibleLow:
        return "reversible_low";
    case ActionRisk::R2UserVisible:
        return "user_visible";
    case ActionRisk::R3Sensitive:
        return "sensitive";
    case ActionRisk::R4Critical:
        return "critical";
    }
    return "read_only";
}

[[nodiscard]] JsonValue capability_list_to_json(const std::vector<CapabilityId> &ids) {
    JsonValue::Array array;
    array.reserve(ids.size());
    for (const auto &id : ids) {
        array.emplace_back(JsonValue{id});
    }
    return JsonValue{std::move(array)};
}

// The digest input of the member ToolSpec digest: module identity plus the
// member's own exposure-relevant contract. Key-order and whitespace free via
// canonical JSON; two manifests declaring the same member under the same
// module identity digest identically.
[[nodiscard]] JsonValue member_spec_to_json(const std::string &module_id,
                                            const ModuleToolSpec &member) {
    JsonValue::Object root;
    root.emplace_back("module_id", module_id);
    root.emplace_back("name", member.name);
    root.emplace_back("version", version_to_json(member.version));
    root.emplace_back("description", member.description);
    root.emplace_back("arguments_schema", member.arguments_schema.root);
    root.emplace_back("result_schema", member.result_schema.root);
    root.emplace_back("required_capabilities",
                      capability_list_to_json(member.required_capabilities));
    root.emplace_back("side_effect", side_effect_name(member.side_effect));
    JsonValue::Object data_access;
    JsonValue::Array reads;
    reads.reserve(member.data_access.reads.size());
    for (const auto &entry : member.data_access.reads) {
        reads.emplace_back(JsonValue{entry});
    }
    JsonValue::Array writes;
    writes.reserve(member.data_access.writes.size());
    for (const auto &entry : member.data_access.writes) {
        writes.emplace_back(JsonValue{entry});
    }
    data_access.emplace_back("reads", JsonValue{std::move(reads)});
    data_access.emplace_back("writes", JsonValue{std::move(writes)});
    root.emplace_back("data_access", JsonValue{std::move(data_access)});
    return JsonValue{std::move(root)};
}

[[nodiscard]] JsonValue module_binding_to_json(const ExposureModuleBinding &binding) {
    JsonValue::Object object;
    object.emplace_back("module_id", binding.module_id);
    object.emplace_back("version", version_to_json(binding.version));
    object.emplace_back("module_digest", binding.module_digest.to_string());
    return JsonValue{std::move(object)};
}

struct ExclusionOrder final {
    bool operator()(const ToolExclusion &lhs, const ToolExclusion &rhs) const noexcept {
        if (lhs.level != rhs.level) {
            return lhs.level < rhs.level;
        }
        return lhs.module_id < rhs.module_id;
    }
};

} // namespace

ToolId derive_module_tool_id(const std::string &module_id, const Hash &module_digest,
                             std::string_view member_name) {
    JsonValue::Object root;
    root.emplace_back("module_id", module_id);
    root.emplace_back("module_digest", module_digest.to_string());
    root.emplace_back("member_name", std::string(member_name));
    const auto digest = canonical_json_digest(JsonValue{std::move(root)});
    Id128::Bytes bytes{};
    std::copy(digest.bytes.begin(), digest.bytes.begin() + bytes.size(), bytes.begin());
    return ToolId{Id128(bytes)};
}

Hash module_tool_spec_digest(const std::string &module_id, const ModuleToolSpec &member) {
    return canonical_json_digest(member_spec_to_json(module_id, member));
}

std::string_view tool_exclusion_reason_name(ToolExclusionReason reason) {
    switch (reason) {
    case ToolExclusionReason::ModuleUnavailable:
        return "module_unavailable";
    case ToolExclusionReason::ModuleConflict:
        return "module_conflict";
    case ToolExclusionReason::ModuleRevoked:
        return "module_revoked";
    case ToolExclusionReason::ReservedWireName:
        return "reserved_wire_name";
    case ToolExclusionReason::TaskPolicy:
        return "task_policy";
    case ToolExclusionReason::TaskBudget:
        return "task_budget";
    }
    return "module_unavailable";
}

Result<ToolExposure> project_tool_exposure(std::uint64_t negotiation_generation,
                                           std::span<const ModuleSnapshot> active_modules,
                                           const ModuleNegotiationResult &negotiation,
                                           const ToolExposureRequest &request,
                                           const ToolExposureLimits &limits) {
    if (active_modules.size() > limits.max_modules) {
        return make_exposure_error(ErrorCode::ResourceExhausted,
                                   "active module set exceeds the projection limit");
    }

    // Task policy exclusions are caller input: validate them fully before any
    // projection state exists so an invalid request can never produce a
    // partial exposure.
    std::map<std::string, std::string> policy_exclusions;
    for (const auto &exclusion : request.excluded_modules) {
        if (exclusion.first.empty()) {
            return make_exposure_error(ErrorCode::InvalidArgument,
                                       "task policy exclusion requires a module_id");
        }
        if (exclusion.second.size() > limits.max_reason_bytes) {
            return make_exposure_error(ErrorCode::InvalidArgument,
                                       "task policy exclusion reason exceeds the limit");
        }
        if (!policy_exclusions.emplace(exclusion.first, exclusion.second).second) {
            return make_exposure_error(ErrorCode::InvalidArgument,
                                       "duplicate task policy exclusion for module '" +
                                           exclusion.first + "'");
        }
    }

    // Pair every active module with its verdict. A set-level mismatch is a
    // whole-projection failure: the caller paired snapshots from different
    // generations, and no availability fact can be trusted (design §8.4).
    std::vector<const ModuleSnapshot *> sorted_modules;
    sorted_modules.reserve(active_modules.size());
    for (const auto &module : active_modules) {
        if (module.module_id.empty()) {
            return make_exposure_error(ErrorCode::InvalidArgument,
                                       "active module set contains an empty module_id");
        }
        sorted_modules.push_back(&module);
    }
    std::sort(sorted_modules.begin(), sorted_modules.end(),
              [](const ModuleSnapshot *lhs, const ModuleSnapshot *rhs) {
                  return lhs->module_id < rhs->module_id;
              });
    for (std::size_t index = 1; index < sorted_modules.size(); ++index) {
        if (sorted_modules[index]->module_id == sorted_modules[index - 1]->module_id) {
            return make_exposure_error(ErrorCode::InvalidArgument,
                                       "active module set contains duplicate module_id '" +
                                           sorted_modules[index]->module_id + "'");
        }
    }

    std::map<std::string, const ModuleAvailability *> verdicts;
    for (const auto &availability : negotiation.modules) {
        if (!verdicts.emplace(availability.module_id, &availability).second) {
            return make_exposure_error(ErrorCode::InvalidArgument,
                                       "negotiation carries duplicate verdicts for module '" +
                                           availability.module_id + "'");
        }
    }
    std::map<std::string, const ModuleSnapshot *> modules_by_id;
    for (const auto *module : sorted_modules) {
        modules_by_id.emplace(module->module_id, module);
    }
    for (const auto &[module_id, availability] : verdicts) {
        const auto found = modules_by_id.find(module_id);
        if (found == modules_by_id.end()) {
            return make_exposure_error(ErrorCode::InvalidArgument,
                                       "negotiation verdict references module '" + module_id +
                                           "' outside the active snapshot");
        }
        if (availability->module_digest != found->second->module_digest ||
            availability->version != found->second->version) {
            return make_exposure_error(
                ErrorCode::InvalidState,
                "negotiation verdict for module '" + module_id +
                    "' does not match the active snapshot digest or version");
        }
    }
    for (const auto *module : sorted_modules) {
        if (verdicts.find(module->module_id) == verdicts.end()) {
            return make_exposure_error(ErrorCode::InvalidState, "active module '" +
                                                                    module->module_id +
                                                                    "' has no negotiation verdict");
        }
    }
    for (const auto &[module_id, reason] : policy_exclusions) {
        if (modules_by_id.find(module_id) == modules_by_id.end()) {
            return make_exposure_error(ErrorCode::InvalidArgument,
                                       "task policy excludes unknown module '" + module_id + "'");
        }
    }

    ToolExposure exposure;
    exposure.negotiation_generation = negotiation_generation;

    std::size_t exposed = 0;
    for (const auto *module : sorted_modules) {
        const ModuleAvailability &availability = *verdicts.at(module->module_id);

        // Module-level negotiation outcomes first (design §8.1).
        if (availability.status == ModuleStatus::Unavailable) {
            ToolExclusion exclusion;
            exclusion.level = ToolExclusionLevel::Module;
            exclusion.reason = ToolExclusionReason::ModuleUnavailable;
            exclusion.module_id = module->module_id;
            exclusion.missing = availability.missing;
            exclusion.detail = "negotiation verdict: unavailable";
            exposure.exclusions.push_back(std::move(exclusion));
            continue;
        }
        if (availability.status == ModuleStatus::Conflict) {
            ToolExclusion exclusion;
            exclusion.level = ToolExclusionLevel::Module;
            exclusion.reason = ToolExclusionReason::ModuleConflict;
            exclusion.module_id = module->module_id;
            exclusion.conflicting_with = availability.conflicting_with;
            exclusion.detail = "negotiation verdict: conflict";
            exposure.exclusions.push_back(std::move(exclusion));
            continue;
        }
        if (availability.status == ModuleStatus::Revoked) {
            ToolExclusion exclusion;
            exclusion.level = ToolExclusionLevel::Module;
            exclusion.reason = ToolExclusionReason::ModuleRevoked;
            exclusion.module_id = module->module_id;
            exclusion.detail = "negotiation verdict: revoked";
            exposure.exclusions.push_back(std::move(exclusion));
            continue;
        }

        // Task policy exclusion (module-grained; members never split).
        if (const auto policy = policy_exclusions.find(module->module_id);
            policy != policy_exclusions.end()) {
            ToolExclusion exclusion;
            exclusion.level = ToolExclusionLevel::Task;
            exclusion.reason = ToolExclusionReason::TaskPolicy;
            exclusion.module_id = module->module_id;
            exclusion.detail = policy->second;
            exposure.exclusions.push_back(std::move(exclusion));
            continue;
        }

        // v1 frozen wire name rule (M7-TM2-02): a member colliding with a
        // hosted provider tool name excludes the whole module — never a
        // rename, never partial members.
        bool reserved = false;
        for (const auto &member : module->tools) {
            if (is_known_hosted_tool_name(member.name)) {
                reserved = true;
                break;
            }
        }
        if (reserved) {
            ToolExclusion exclusion;
            exclusion.level = ToolExclusionLevel::Module;
            exclusion.reason = ToolExclusionReason::ReservedWireName;
            exclusion.module_id = module->module_id;
            exclusion.detail = "a member name collides with a hosted provider tool name";
            exposure.exclusions.push_back(std::move(exclusion));
            continue;
        }

        // Exposure budget, evaluated in module_id order; a module that does
        // not fit the remaining budget is excluded whole (TaskBudget).
        if (exposed + module->tools.size() > request.max_exposed_tools) {
            ToolExclusion exclusion;
            exclusion.level = ToolExclusionLevel::Task;
            exclusion.reason = ToolExclusionReason::TaskBudget;
            exclusion.module_id = module->module_id;
            exclusion.detail = "exposure budget exhausted before the module fit";
            exposure.exclusions.push_back(std::move(exclusion));
            continue;
        }

        for (const auto &member : module->tools) {
            ExposedToolSpec tool;
            tool.tool_id =
                derive_module_tool_id(module->module_id, module->module_digest, member.name);
            tool.version = member.version;
            tool.wire_name = member.name;
            tool.description = member.description;
            tool.parameters_schema = member.arguments_schema;
            tool.spec_digest = module_tool_spec_digest(module->module_id, member);
            tool.has_side_effects = member.side_effect != ActionRisk::R0ReadOnly;
            exposure.tools.push_back(std::move(tool));
        }
        exposed += module->tools.size();

        ExposureModuleBinding binding;
        binding.module_id = module->module_id;
        binding.version = module->version;
        binding.module_digest = module->module_digest;
        exposure.modules.push_back(std::move(binding));
    }

    if (exposure.tools.size() > limits.max_tools) {
        return make_exposure_error(ErrorCode::ResourceExhausted,
                                   "projected tool count exceeds the projection limit");
    }

    // Defense in depth behind the TM0/TM1 wire namespace gates: duplicates
    // reaching the projection are a whole-projection failure, never a
    // silently merged or overwritten exposure.
    std::sort(exposure.tools.begin(), exposure.tools.end(),
              [](const ExposedToolSpec &lhs, const ExposedToolSpec &rhs) {
                  return lhs.wire_name < rhs.wire_name;
              });
    std::set<ToolId> seen_ids;
    for (std::size_t index = 0; index < exposure.tools.size(); ++index) {
        if (index > 0 && exposure.tools[index].wire_name == exposure.tools[index - 1].wire_name) {
            return make_exposure_error(ErrorCode::InvalidState,
                                       "duplicate wire name reached the projection: '" +
                                           exposure.tools[index].wire_name + "'");
        }
        if (!seen_ids.insert(exposure.tools[index].tool_id).second) {
            return make_exposure_error(ErrorCode::InvalidState,
                                       "duplicate derived tool id reached the projection");
        }
    }

    std::sort(exposure.exclusions.begin(), exposure.exclusions.end(), ExclusionOrder{});

    // Composite snapshot digest: the negotiation generation, the included
    // module digest set and the member (tool_id, spec_digest, wire_name)
    // triples. Exclusions are decision records, not binding facts, and stay
    // out of the digest.
    JsonValue::Array digest_modules;
    digest_modules.reserve(exposure.modules.size());
    for (const auto &binding : exposure.modules) {
        JsonValue::Object object;
        object.emplace_back("module_id", binding.module_id);
        object.emplace_back("module_digest", binding.module_digest.to_string());
        digest_modules.emplace_back(JsonValue{std::move(object)});
    }
    JsonValue::Array digest_tools;
    digest_tools.reserve(exposure.tools.size());
    for (const auto &tool : exposure.tools) {
        JsonValue::Object object;
        object.emplace_back("tool_id", tool.tool_id.to_string());
        object.emplace_back("spec_digest", tool.spec_digest.to_string());
        object.emplace_back("wire_name", tool.wire_name);
        digest_tools.emplace_back(JsonValue{std::move(object)});
    }
    JsonValue::Object digest_root;
    digest_root.emplace_back("schema", std::string(kToolExposureSchema));
    digest_root.emplace_back("negotiation_generation",
                             static_cast<std::int64_t>(negotiation_generation));
    digest_root.emplace_back("modules", JsonValue{std::move(digest_modules)});
    digest_root.emplace_back("tools", JsonValue{std::move(digest_tools)});
    exposure.snapshot_digest = canonical_json_digest(JsonValue{std::move(digest_root)});

    return exposure;
}

JsonValue tool_exposure_to_json(const ToolExposure &exposure) {
    JsonValue::Array modules;
    modules.reserve(exposure.modules.size());
    for (const auto &binding : exposure.modules) {
        modules.emplace_back(module_binding_to_json(binding));
    }

    JsonValue::Array tools;
    tools.reserve(exposure.tools.size());
    for (const auto &tool : exposure.tools) {
        JsonValue::Object object;
        object.emplace_back("tool_id", tool.tool_id.to_string());
        object.emplace_back("wire_name", tool.wire_name);
        object.emplace_back("version", version_to_json(tool.version));
        object.emplace_back("spec_digest", tool.spec_digest.to_string());
        object.emplace_back("has_side_effects", tool.has_side_effects);
        tools.emplace_back(JsonValue{std::move(object)});
    }

    JsonValue::Array exclusions;
    exclusions.reserve(exposure.exclusions.size());
    for (const auto &exclusion : exposure.exclusions) {
        JsonValue::Object object;
        object.emplace_back("level", exclusion.level == ToolExclusionLevel::Module
                                         ? std::string("module")
                                         : std::string("task"));
        object.emplace_back("reason", std::string(tool_exclusion_reason_name(exclusion.reason)));
        object.emplace_back("module_id", exclusion.module_id);
        object.emplace_back("missing", capability_list_to_json(exclusion.missing));
        object.emplace_back("conflicting_with", exclusion.conflicting_with);
        object.emplace_back("detail", exclusion.detail);
        exclusions.emplace_back(JsonValue{std::move(object)});
    }

    JsonValue::Object root;
    root.emplace_back("schema", std::string(kToolExposureSchema));
    root.emplace_back("negotiation_generation",
                      static_cast<std::int64_t>(exposure.negotiation_generation));
    root.emplace_back("snapshot_digest", exposure.snapshot_digest.to_string());
    root.emplace_back("modules", JsonValue{std::move(modules)});
    root.emplace_back("tools", JsonValue{std::move(tools)});
    root.emplace_back("exclusions", JsonValue{std::move(exclusions)});
    return JsonValue{std::move(root)};
}

Result<void> verify_recorded_module_digests(std::span<const Hash> recorded,
                                            const ToolExposure &exposure) {
    std::vector<Hash> expected;
    expected.reserve(exposure.modules.size());
    for (const auto &binding : exposure.modules) {
        expected.push_back(binding.module_digest);
    }
    std::vector<Hash> provided(recorded.begin(), recorded.end());
    const auto by_bytes = [](const Hash &lhs, const Hash &rhs) { return lhs.bytes < rhs.bytes; };
    std::sort(expected.begin(), expected.end(), by_bytes);
    std::sort(provided.begin(), provided.end(), by_bytes);
    if (expected == provided) {
        return Result<void>{};
    }
    return make_exposure_error(ErrorCode::InvalidState,
                               "recorded module digest set does not match the exposure");
}

Result<ToolModuleManifest> make_simulator_reference_module() {
    JsonValue::Object describe;
    describe.emplace_back("name", std::string("simulator.describe_display"));
    describe.emplace_back("version", version_to_json(SemanticVersion{1, 0, 0}));
    describe.emplace_back("description", std::string("Describe the simulated display topology."));
    describe.emplace_back("arguments_schema",
                          JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}}}});
    describe.emplace_back("result_schema",
                          JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}}}});
    describe.emplace_back("side_effect", std::string("read_only"));

    JsonValue::Object inject;
    inject.emplace_back("name", std::string("simulator.inject_tap"));
    inject.emplace_back("version", version_to_json(SemanticVersion{1, 0, 0}));
    inject.emplace_back(
        "description",
        std::string("Inject a tap at a logical display coordinate in the simulator."));
    inject.emplace_back("arguments_schema",
                        JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}}}});
    inject.emplace_back("result_schema",
                        JsonValue{JsonValue::Object{{"type", JsonValue{std::string("object")}}}});
    inject.emplace_back("required_capabilities",
                        JsonValue{JsonValue::Array{JsonValue{std::string("env.input.discrete")}}});
    inject.emplace_back("side_effect", std::string("user_visible"));

    JsonValue::Array tools;
    tools.emplace_back(JsonValue{std::move(describe)});
    tools.emplace_back(JsonValue{std::move(inject)});

    JsonValue::Object origin;
    origin.emplace_back("isolation", std::string("built_in"));

    JsonValue::Object resources;
    resources.emplace_back("max_total_concurrent_invocations", std::int64_t{2});
    resources.emplace_back("max_total_result_bytes", std::int64_t{4096});

    JsonValue::Object root;
    root.emplace_back("schema", std::string(kToolModuleManifestSchema));
    root.emplace_back("schema_version", std::string(kToolModuleManifestSchemaVersion));
    root.emplace_back("module_id", std::string("builtin.simulator.env"));
    root.emplace_back("version", version_to_json(SemanticVersion{1, 0, 0}));
    root.emplace_back("origin", JsonValue{std::move(origin)});
    root.emplace_back("tools", JsonValue{std::move(tools)});
    root.emplace_back("resources", JsonValue{std::move(resources)});

    return parse_tool_module_manifest(JsonValue{std::move(root)}, CapabilityCatalog::core());
}

} // namespace mira
