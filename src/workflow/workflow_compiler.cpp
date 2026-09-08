#include <mira/workflow_compiler.hpp>

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <utility>

namespace mira {
namespace {

constexpr std::size_t kMinInductionTrajectories = 2;
constexpr std::size_t kMaxInductionTrajectories = 16;

[[nodiscard]] bool scalar_kind(JsonValue::Kind kind) {
    return kind == JsonValue::Kind::Boolean || kind == JsonValue::Kind::Integer ||
           kind == JsonValue::Kind::Number || kind == JsonValue::Kind::String;
}

// RFC 6901 tokens: "" -> root, "/a/0/b" -> {a, 0, b}; ~1 -> /, ~0 -> ~.
[[nodiscard]] std::optional<std::vector<std::string>>
tokenize_pointer(const std::string &pointer, std::size_t max_bytes) {
    if (pointer.size() > max_bytes) {
        return std::nullopt;
    }
    if (pointer.empty()) {
        return std::vector<std::string>{};
    }
    if (pointer.front() != '/') {
        return std::nullopt;
    }
    std::vector<std::string> tokens;
    std::size_t start = 1;
    while (true) {
        const auto end = pointer.find('/', start);
        std::string token = pointer.substr(start, end == std::string::npos
                                                       ? std::string::npos
                                                       : end - start);
        // Unescape in one pass: ~1 before ~0 per RFC 6901.
        std::string unescaped;
        for (std::size_t index = 0; index < token.size(); ++index) {
            if (token[index] == '~' && index + 1 < token.size() &&
                (token[index + 1] == '0' || token[index + 1] == '1')) {
                unescaped.push_back(token[index + 1] == '1' ? '/' : '~');
                ++index;
                continue;
            }
            unescaped.push_back(token[index]);
        }
        // A malformed escape never matches a member, so resolution of such a
        // pointer fails closed later.
        tokens.push_back(std::move(unescaped));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return tokens;
}

[[nodiscard]] std::string escape_token(const std::string &token) {
    std::string escaped;
    for (const char character : token) {
        if (character == '~') {
            escaped += "~0";
        } else if (character == '/') {
            escaped += "~1";
        } else {
            escaped.push_back(character);
        }
    }
    return escaped;
}

[[nodiscard]] const JsonValue *resolve_pointer(const JsonValue &root,
                                               const std::vector<std::string> &tokens) {
    const JsonValue *current = &root;
    for (const auto &token : tokens) {
        if (current->is_object()) {
            current = current->find(token);
        } else if (current->is_array()) {
            if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos) {
                return nullptr;
            }
            const std::size_t index = static_cast<std::size_t>(std::stoull(token));
            const auto *array = current->as_array();
            if (index >= array->size()) {
                return nullptr;
            }
            current = &(*array)[index];
        } else {
            return nullptr;
        }
        if (current == nullptr) {
            return nullptr;
        }
    }
    return current;
}

// Functionally rewrites the leaf a pointer addresses; nullopt when the path
// does not resolve (the caller resolved it read-only first, so this marks a
// genuine invariant break, not a candidate-shape problem).
[[nodiscard]] std::optional<JsonValue> rewrite_leaf(const JsonValue &value,
                                                    const std::vector<std::string> &tokens,
                                                    const JsonValue &replacement) {
    if (tokens.empty()) {
        return replacement;
    }
    const std::string &token = tokens.front();
    std::vector<std::string> rest(tokens.begin() + 1, tokens.end());
    if (value.is_object()) {
        JsonValue::Object object;
        bool replaced = false;
        for (const auto &member : *value.as_object()) {
            JsonValue child = member.second;
            if (member.first == token) {
                auto rewritten = rewrite_leaf(child, rest, replacement);
                if (!rewritten.has_value()) {
                    return std::nullopt;
                }
                child = std::move(rewritten.value());
                replaced = true;
            }
            object.emplace_back(member.first, std::move(child));
        }
        if (!replaced) {
            return std::nullopt;
        }
        return JsonValue{std::move(object)};
    }
    if (value.is_array()) {
        if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos) {
            return std::nullopt;
        }
        const std::size_t index = static_cast<std::size_t>(std::stoull(token));
        const auto *items = value.as_array();
        if (index >= items->size()) {
            return std::nullopt;
        }
        JsonValue::Array array;
        array.reserve(items->size());
        for (std::size_t position = 0; position < items->size(); ++position) {
            JsonValue child = (*items)[position];
            if (position == index) {
                auto rewritten = rewrite_leaf(child, rest, replacement);
                if (!rewritten.has_value()) {
                    return std::nullopt;
                }
                child = std::move(rewritten.value());
            }
            array.push_back(std::move(child));
        }
        return JsonValue{std::move(array)};
    }
    return std::nullopt;
}

[[nodiscard]] bool is_param_reference(const JsonValue &value, std::string &name) {
    if (!value.is_object()) {
        return false;
    }
    const auto *object = value.as_object();
    if (object->size() != 1 || object->front().first != "$param" ||
        !object->front().second.is_string()) {
        return false;
    }
    name = *object->front().second.as_string();
    return true;
}

[[nodiscard]] std::optional<WorkflowParameterType> scalar_parameter_type(const JsonValue &value) {
    switch (value.kind()) {
    case JsonValue::Kind::Boolean:
        return WorkflowParameterType::Boolean;
    case JsonValue::Kind::Integer:
        return WorkflowParameterType::Integer;
    case JsonValue::Kind::Number:
        return WorkflowParameterType::Number;
    case JsonValue::Kind::String:
        return WorkflowParameterType::String;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] bool valid_parameter_name(const std::string &name) {
    static const std::regex pattern{"^[a-z][a-z0-9_]{0,63}$"};
    return std::regex_search(name, pattern);
}

// Collects the provenance map (pointer -> parameter name) of one trajectory
// step: leaves of the effective tree whose raw counterpart is a {"$param"}
// reference (DEC-026 §1).
void collect_provenance(const JsonValue &raw, const JsonValue &effective,
                        const std::string &pointer,
                        std::map<std::string, std::string> &provenance) {
    std::string name;
    if (is_param_reference(raw, name)) {
        provenance.emplace(pointer, std::move(name));
        return;
    }
    if (raw.is_object() && effective.is_object()) {
        for (const auto &member : *raw.as_object()) {
            if (member.first == "$param") {
                continue;
            }
            if (const JsonValue *child = effective.find(member.first); child != nullptr) {
                collect_provenance(member.second, *child, pointer + "/" + escape_token(member.first),
                                   provenance);
            }
        }
        return;
    }
    if (raw.is_array() && effective.is_array()) {
        const auto *raw_items = raw.as_array();
        const auto *items = effective.as_array();
        for (std::size_t index = 0; index < raw_items->size() && index < items->size(); ++index) {
            collect_provenance((*raw_items)[index], (*items)[index],
                               pointer + "/" + std::to_string(index), provenance);
        }
    }
}

// Shape compatibility (DEC-026 §1): same tree shape — same key sets, same
// array lengths and same internal node kinds, recursively. Scalar kinds may
// differ (reported as a type mismatch later); a scalar facing a container is
// a skeleton violation.
[[nodiscard]] Result<void> check_shape(const JsonValue &left, const JsonValue &right,
                                       const std::string &where) {
    if (left.is_object() && right.is_object()) {
        const auto *left_object = left.as_object();
        const auto *right_object = right.as_object();
        if (left_object->size() != right_object->size()) {
            return make_workflow_compile_error(WorkflowCompileError::InductSkeletonMismatch,
                                               "trajectory objects differ in key count at " +
                                                   where);
        }
        for (const auto &member : *left_object) {
            const JsonValue *counterpart = right.find(member.first);
            if (counterpart == nullptr) {
                return make_workflow_compile_error(WorkflowCompileError::InductSkeletonMismatch,
                                                   "trajectory objects differ in keys at " + where);
            }
            if (auto nested = check_shape(member.second, *counterpart,
                                          where + "/" + escape_token(member.first));
                !nested.has_value()) {
                return nested.error();
            }
        }
        return Result<void>{};
    }
    if (left.is_array() && right.is_array()) {
        const auto *left_items = left.as_array();
        const auto *right_items = right.as_array();
        if (left_items->size() != right_items->size()) {
            return make_workflow_compile_error(WorkflowCompileError::InductSkeletonMismatch,
                                               "trajectory arrays differ in length at " + where);
        }
        for (std::size_t index = 0; index < left_items->size(); ++index) {
            if (auto nested = check_shape((*left_items)[index], (*right_items)[index],
                                          where + "/" + std::to_string(index));
                !nested.has_value()) {
                return nested.error();
            }
        }
        return Result<void>{};
    }
    const bool left_container = left.is_object() || left.is_array();
    const bool right_container = right.is_object() || right.is_array();
    if (left_container != right_container) {
        return make_workflow_compile_error(WorkflowCompileError::InductSkeletonMismatch,
                                           "trajectory arguments differ in shape at " + where);
    }
    return Result<void>{};
}

// Enumerates the scalar leaf sites of one tree in deterministic walk order.
void enumerate_leaves(const JsonValue &value, const std::string &pointer,
                      std::vector<std::string> &sites) {
    if (value.is_object()) {
        for (const auto &member : *value.as_object()) {
            enumerate_leaves(member.second, pointer + "/" + escape_token(member.first), sites);
        }
        return;
    }
    if (value.is_array()) {
        const auto *items = value.as_array();
        for (std::size_t index = 0; index < items->size(); ++index) {
            enumerate_leaves((*items)[index], pointer + "/" + std::to_string(index), sites);
        }
        return;
    }
    sites.push_back(pointer);
}

} // namespace

Error make_workflow_compile_error(WorkflowCompileError code, std::string detail) {
    Error error;
    error.domain = "mira.workflow";
    error.domain_code = static_cast<std::int32_t>(code);
    error.safe_message = std::move(detail);
    return error;
}

std::string workflow_candidate_provenance_name(WorkflowCandidateProvenance provenance) {
    switch (provenance) {
    case WorkflowCandidateProvenance::Provenance:
        return "provenance";
    case WorkflowCandidateProvenance::Structural:
        return "structural";
    case WorkflowCandidateProvenance::Explicit:
        return "explicit";
    }
    return "unknown";
}

Result<WorkflowCandidateProvenance>
parse_workflow_candidate_provenance(std::string_view name) {
    for (const auto provenance : {WorkflowCandidateProvenance::Provenance,
                                  WorkflowCandidateProvenance::Structural,
                                  WorkflowCandidateProvenance::Explicit}) {
        if (workflow_candidate_provenance_name(provenance) == name) {
            return provenance;
        }
    }
    return make_workflow_compile_error(WorkflowCompileError::InductCandidateInvalid,
                                       "unknown candidate provenance name");
}

Result<std::vector<WorkflowParameterCandidate>>
induce_parameters(const std::vector<WorkflowTrajectory> &trajectories,
                  const WorkflowLimits &limits) {
    if (trajectories.size() < kMinInductionTrajectories ||
        trajectories.size() > kMaxInductionTrajectories) {
        return make_workflow_compile_error(WorkflowCompileError::InductCandidateInvalid,
                                           "structural induction requires 2..16 trajectories");
    }
    const std::size_t step_count = trajectories.front().steps.size();
    for (const auto &trajectory : trajectories) {
        if (trajectory.steps.size() != step_count) {
            return make_workflow_compile_error(WorkflowCompileError::InductSkeletonMismatch,
                                               "trajectories differ in step count");
        }
        for (std::size_t index = 0; index < step_count; ++index) {
            const auto &anchor = trajectories.front().steps[index];
            const auto &other = trajectory.steps[index];
            if (anchor.kind != other.kind || anchor.name != other.name) {
                return make_workflow_compile_error(WorkflowCompileError::InductSkeletonMismatch,
                                                   "trajectories differ in step skeleton");
            }
            if (auto shape = check_shape(anchor.arguments, other.arguments,
                                         "step " + std::to_string(index));
                !shape.has_value()) {
                return shape.error();
            }
        }
    }

    std::vector<WorkflowParameterCandidate> candidates;
    // Site keys group per step; provenance names win over generated ones.
    std::set<std::string> used_names;
    for (const auto &spec : trajectories.front().source_parameters) {
        used_names.insert(spec.name);
    }
    std::size_t serial = 0;
    for (std::size_t index = 0; index < step_count; ++index) {
        const auto &anchor = trajectories.front().steps[index];
        std::map<std::string, std::string> provenance;
        collect_provenance(anchor.raw_arguments, anchor.arguments, "", provenance);

        std::vector<std::string> sites;
        enumerate_leaves(anchor.arguments, "", sites);
        for (const auto &site : sites) {
            const auto tokens = tokenize_pointer(site, limits.max_string_bytes);
            if (!tokens.has_value()) {
                continue;
            }
            std::vector<JsonValue> observed;
            observed.reserve(trajectories.size());
            JsonValue::Kind kind = JsonValue::Kind::Null;
            bool kind_known = false;
            bool leaf_missing = false;
            for (const auto &trajectory : trajectories) {
                const JsonValue *value =
                    resolve_pointer(trajectory.steps[index].arguments, tokens.value());
                if (value == nullptr) {
                    leaf_missing = true;
                    break;
                }
                if (!kind_known) {
                    kind = value->kind();
                    kind_known = true;
                } else if (value->kind() != kind) {
                    return make_workflow_compile_error(
                        WorkflowCompileError::InductTypeMismatch,
                        "observed values differ in type at step " + std::to_string(index) +
                            " pointer " + site);
                }
                observed.push_back(*value);
            }
            if (leaf_missing) {
                return make_workflow_compile_error(WorkflowCompileError::InductSkeletonMismatch,
                                                   "leaf vanished during induction walk");
            }
            if (!scalar_kind(kind)) {
                continue; // null leaves are constants, not candidates (DEC-026 §1)
            }
            const bool differs = std::any_of(observed.begin() + 1, observed.end(),
                                             [&](const JsonValue &value) {
                                                 return !(value == observed.front());
                                             });
            if (!differs) {
                continue;
            }
            WorkflowParameterCandidate candidate;
            candidate.step_index = index;
            candidate.pointer = site;
            const auto known = provenance.find(site);
            if (known != provenance.end()) {
                // A provenance name legitimately equals its source parameter
                // (the compile step reuses that spec); multiple steps may
                // reference the same parameter.
                candidate.name = known->second;
                candidate.provenance = WorkflowCandidateProvenance::Provenance;
            } else {
                do {
                    ++serial;
                } while (used_names.count("param_" + std::to_string(serial)) != 0);
                candidate.name = "param_" + std::to_string(serial);
                candidate.provenance = WorkflowCandidateProvenance::Structural;
            }
            used_names.insert(candidate.name);
            candidate.observed_values = std::move(observed);
            candidates.push_back(std::move(candidate));
        }
    }
    if (candidates.size() > limits.max_parameters) {
        return make_workflow_compile_error(WorkflowCompileError::InductLimitExceeded,
                                           "induced candidates exceed the parameter budget");
    }
    return candidates;
}

Result<WorkflowDefinition>
compile_workflow(const WorkflowTrajectory &trajectory, const WorkflowCompileOptions &options) {
    return compile_workflow(trajectory, options, {}, kDefaultWorkflowLimits);
}

Result<WorkflowDefinition>
compile_workflow(const WorkflowTrajectory &trajectory, const WorkflowCompileOptions &options,
                 const std::vector<WorkflowParameterCandidate> &candidates,
                 const WorkflowLimits &limits) {
    if (options.workflow_id.is_nil() || options.name.empty()) {
        return make_workflow_compile_error(WorkflowCompileError::CompileInvalidOptions,
                                           "compile options require a workflow id and a name");
    }
    WorkflowPolicy default_policy =
        options.default_policy.value_or(trajectory.source_default_policy);
    std::vector<WorkflowPolicy> allowed = trajectory.allowed_policies;
    if (allowed.empty()) {
        allowed.push_back(default_policy);
    }
    if (std::find(allowed.begin(), allowed.end(), default_policy) == allowed.end()) {
        return make_workflow_compile_error(
            WorkflowCompileError::CompilePolicyNotAllowed,
            "default policy is not a member of the effective allowed set");
    }

    WorkflowDefinition definition;
    definition.schema_version = SchemaVersion{1, 0};
    definition.workflow_id = options.workflow_id;
    definition.name = options.name;
    definition.summary = options.summary;
    definition.default_policy = default_policy;
    definition.allowed_policies = std::move(allowed);

    // Parameters first (DEC-025 §2): observed values bake as defaults and
    // required flips to optional, since a default-bearing parameter is never
    // required.
    definition.parameters = trajectory.source_parameters;
    const auto *effective = trajectory.effective_parameters.as_object();
    for (auto &spec : definition.parameters) {
        const JsonValue *observed = nullptr;
        if (effective != nullptr) {
            for (const auto &member : *effective) {
                if (member.first == spec.name) {
                    observed = &member.second;
                    break;
                }
            }
        }
        if (observed != nullptr) {
            spec.default_value = *observed;
            spec.required = false;
        }
    }

    // Steps: effective arguments verbatim; skipped steps are already dropped
    // by capture, so a Control jump that no longer resolves fails closed.
    definition.steps.reserve(trajectory.steps.size());
    std::set<std::string> kept_ids;
    for (const auto &source : trajectory.steps) {
        WorkflowStep step;
        step.id = source.source_step_id;
        step.name = source.name;
        step.kind = source.kind;
        step.loop_head = source.loop_head;
        step.arguments = source.arguments;
        step.precondition = source.precondition;
        step.verification = source.verification;
        step.recovery = source.recovery;
        step.max_attempts = source.max_attempts;
        step.jump_to = source.jump_to;
        step.max_iterations = source.max_iterations;
        if (step.kind == WorkflowStepKind::ToolCall) {
            const auto *tool = step.arguments.find("tool");
            if (!step.arguments.is_object() || tool == nullptr || !tool->is_string() ||
                tool->as_string()->empty()) {
                return make_workflow_compile_error(
                    WorkflowCompileError::CompileToolBindingInvalid,
                    "effective tool_call arguments must be an object naming the target tool in "
                    "the reserved \"tool\" member");
            }
        }
        kept_ids.insert(step.id.to_string());
        definition.steps.push_back(std::move(step));
    }
    for (const auto &step : definition.steps) {
        if (step.kind == WorkflowStepKind::Control && step.jump_to.has_value() &&
            kept_ids.count(step.jump_to->to_string()) == 0) {
            return make_workflow_compile_error(
                WorkflowCompileError::CompileJumpTargetSkipped,
                "control jump targets a step dropped from the compiled definition");
        }
    }

    // Candidates (DEC-026 §2): validate, rewrite leaves, add inferred specs.
    // Provenance candidates reuse the source parameter spec (same name);
    // anything else with that name is a conflict.
    std::map<std::string, std::vector<const WorkflowParameterCandidate *>> by_name;
    for (const auto &candidate : candidates) {
        if (!valid_parameter_name(candidate.name) || candidate.pointer.empty()) {
            return make_workflow_compile_error(
                WorkflowCompileError::InductCandidateInvalid,
                "candidate name must match ^[a-z][a-z0-9_]{0,63}$ and the pointer must be "
                "non-empty");
        }
        if (candidate.step_index >= trajectory.steps.size()) {
            return make_workflow_compile_error(WorkflowCompileError::InductCandidateInvalid,
                                               "candidate step index is out of range");
        }
        if (candidate.provenance != WorkflowCandidateProvenance::Provenance &&
            candidate.provenance != WorkflowCandidateProvenance::Structural &&
            candidate.provenance != WorkflowCandidateProvenance::Explicit) {
            return make_workflow_compile_error(WorkflowCompileError::InductCandidateInvalid,
                                               "candidate provenance is unknown");
        }
        const auto tokens = tokenize_pointer(candidate.pointer, limits.max_string_bytes);
        if (!tokens.has_value()) {
            return make_workflow_compile_error(WorkflowCompileError::InductCandidateInvalid,
                                               "candidate pointer is not a valid RFC 6901 form");
        }
        const JsonValue &arguments = trajectory.steps[candidate.step_index].arguments;
        const JsonValue *leaf = resolve_pointer(arguments, tokens.value());
        if (leaf == nullptr || !scalar_kind(leaf->kind())) {
            return make_workflow_compile_error(WorkflowCompileError::InductLeafNotScalar,
                                               "candidate pointer must resolve to a scalar leaf");
        }
        if (trajectory.steps[candidate.step_index].kind == WorkflowStepKind::ToolCall &&
            !tokens.value().empty() && tokens.value().back() == "tool") {
            return make_workflow_compile_error(WorkflowCompileError::InductReservedMember,
                                               "candidate pointer targets the reserved \"tool\" "
                                               "member");
        }
        by_name[candidate.name].push_back(&candidate);
    }
    for (const auto &[name, group] : by_name) {
        const bool provenance = std::all_of(
            group.begin(), group.end(), [&](const WorkflowParameterCandidate *candidate) {
                return candidate->provenance == WorkflowCandidateProvenance::Provenance;
            });
        const bool declared = std::any_of(trajectory.source_parameters.begin(),
                                          trajectory.source_parameters.end(),
                                          [&](const WorkflowParameterSpec &spec) {
                                              return spec.name == name;
                                          });
        if (provenance && !declared) {
            return make_workflow_compile_error(
                WorkflowCompileError::InductCandidateInvalid,
                "provenance candidate '" + name + "' does not match a declared parameter");
        }
        if (!provenance && (declared || group.size() > 1)) {
            return make_workflow_compile_error(
                WorkflowCompileError::InductNameConflict,
                "candidate name '" + name + "' collides with a parameter or another candidate");
        }
    }

    for (const auto &candidate : candidates) {
        const auto tokens = tokenize_pointer(candidate.pointer, limits.max_string_bytes).value();
        const JsonValue replacement(JsonValue::Object{{"$param", JsonValue{candidate.name}}});
        auto rewritten = rewrite_leaf(definition.steps[candidate.step_index].arguments, tokens,
                                      replacement);
        if (!rewritten.has_value()) {
            return make_workflow_compile_error(WorkflowCompileError::InductLeafNotScalar,
                                               "candidate leaf vanished during rewrite");
        }
        definition.steps[candidate.step_index].arguments = std::move(rewritten.value());

        const bool declared = std::any_of(
            trajectory.source_parameters.begin(), trajectory.source_parameters.end(),
            [&](const WorkflowParameterSpec &spec) { return spec.name == candidate.name; });
        if (declared) {
            continue; // Provenance candidate: the baked source spec is reused.
        }
        JsonValue anchor = !candidate.observed_values.empty()
                               ? candidate.observed_values.front()
                               : *resolve_pointer(trajectory.steps[candidate.step_index].arguments,
                                                  tokens);
        const auto type = scalar_parameter_type(anchor);
        if (!type.has_value()) {
            return make_workflow_compile_error(WorkflowCompileError::InductCandidateInvalid,
                                               "candidate anchor observation is not a scalar");
        }
        WorkflowParameterSpec spec;
        spec.name = candidate.name;
        spec.type = type.value();
        spec.required = false;
        spec.default_value = std::move(anchor);
        spec.summary = "induced by " + workflow_candidate_provenance_name(candidate.provenance) +
                       " induction";
        definition.parameters.push_back(std::move(spec));
        if (definition.parameters.size() > limits.max_parameters) {
            return make_workflow_compile_error(WorkflowCompileError::InductLimitExceeded,
                                               "compiled parameters exceed the budget");
        }
    }

    if (auto valid = validate_workflow_definition(definition, limits); !valid.has_value()) {
        return valid.error();
    }
    return definition;
}

} // namespace mira
