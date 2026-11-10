#include "memory_preference_support.hpp"

namespace mira {
BuiltinToolRegistration preference_proposal_tool(
    std::function<Result<PreferenceProposal>(PreferenceProposalRequest, const OperationContext &)>
        propose) {
    BuiltinToolSpec spec;
    spec.wire_name = "memory_propose_preference";
    spec.description = "Propose one user-scoped key=value preference for explicit human review. "
                       "Does not approve, persist a MemoryRecord, or change verification. "
                       "A correction freezes the previous record and version for review.";
    spec.has_side_effects = true;
    spec.parameters_schema = JsonSchema{
        parse_json(
            R"({"type":"object","properties":{"statement":{"type":"string","minLength":3,"maxLength":4096},"request_id":{"type":"string"},"target":{"type":"string"},"expected_version":{"type":"integer","minimum":1}},"required":["statement"],"additionalProperties":false})")
            .value()};
    return {spec,
            [propose = std::move(propose)](const JsonValue &args,
                                           const OperationContext &context) -> Result<JsonValue> {
                if (context.operation.is_nil())
                    return memory_agent_support::error(ErrorCode::InvalidArgument,
                                                       "preference tool requires an operation");
                PreferenceProposalRequest request;
                request.id = memory_preference::identity<MutationId>(
                    "mira.preference.request.v1/", MutationId{context.operation.value});
                const auto *statement = memory_preference::field(args, "statement");
                if (!statement)
                    return memory_agent_support::error(ErrorCode::InvalidArgument,
                                                       "preference statement is missing");
                request.statement = *statement;
                if (const auto *value = args.find("request_id")) {
                    const auto id =
                        value->is_string() ? MutationId::parse(*value->as_string()) : std::nullopt;
                    if (!id)
                        return memory_agent_support::error(ErrorCode::InvalidArgument,
                                                           "invalid preference request id");
                    request.id = *id;
                }
                if (const auto *value = args.find("target")) {
                    const auto id =
                        value->is_string() ? MemoryId::parse(*value->as_string()) : std::nullopt;
                    if (!id)
                        return memory_agent_support::error(ErrorCode::InvalidArgument,
                                                           "invalid preference target");
                    request.target = *id;
                }
                if (const auto *value = args.find("expected_version")) {
                    if (!value->is_integer() || *value->as_integer() < 1)
                        return memory_agent_support::error(ErrorCode::InvalidArgument,
                                                           "invalid preference version");
                    request.expected_version = static_cast<std::uint64_t>(*value->as_integer());
                }
                auto proposal = propose(std::move(request), context);
                if (!proposal)
                    return proposal.error();
                JsonValue::Object result{
                    {"proposal_id", proposal.value().id.to_string()},
                    {"digest", proposal.value().digest.to_string()},
                    {"needs_human_approval",
                     proposal.value().state == PreferenceProposalState::Pending},
                    {"state", static_cast<std::int64_t>(proposal.value().state)}};
                if (proposal.value().previous) {
                    result.emplace_back("previous_id", proposal.value().previous->id.to_string());
                    result.emplace_back("previous_version",
                                        std::to_string(proposal.value().previous->version));
                }
                return JsonValue{std::move(result)};
            }};
}
} // namespace mira
