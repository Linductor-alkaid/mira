#include <mira/model_replay.hpp>
#include <mira/model_digest.hpp>

#include <utility>

namespace mira {

ReplayModelProvider::ReplayModelProvider(std::shared_ptr<const ModelProfile> profile,
                                         std::vector<ModelResponse> script)
    : profile_(std::move(profile)), script_(std::move(script)) {}

Result<ModelResponse> ReplayModelProvider::infer(const ModelRequest &request,
                                                 const OperationContext &context,
                                                 const ProviderInferOptions &options) {
    if (context.cancelled()) {
        return make_model_error(ModelDomainCode::ModelCancelled,
                                "replay was cancelled", false, request.operation_id);
    }
    if (options.stream) {
        // Recorded responses are terminal canonical objects; replay never
        // re-streams, so a stream request is a capability mismatch.
        return make_model_error(ModelDomainCode::CapabilityMismatch,
                                "replay does not provide streaming", false,
                                request.operation_id);
    }
    if (cursor_ >= script_.size()) {
        return make_model_error(ModelDomainCode::ModelResourceExhausted,
                                "replay script is exhausted", false, request.operation_id);
    }
    ModelResponse response = script_[cursor_];
    cursor_ += 1;
    // Rebind the recorded response to the live request identity so downstream
    // validation and event correlation stay coherent.
    response.request_id = request.request_id;
    response.operation_id = request.operation_id;
    response.profile_id = request.profile_id;
    if (response.protected_raw_response.has_value() && raw_erased_) {
        raw_missing_ = true;
    }
    served_.push_back(model_request_canonical_digest(request));
    return response;
}

} // namespace mira
