#include <mira/adapters/simulator/simulator_environment.hpp>

#include <cstdint>
#include <mutex>
#include <utility>

namespace mira::adapters::simulator {

class SimulatorEnvironment::Impl final {
public:
    mutable std::mutex mutex;
    std::string content;
    std::vector<InputEvent> actions;
    std::uint64_t observation_sequence = 0;
    std::uint64_t execution_sequence = 0;
    bool was_interrupted = false;
};

SimulatorEnvironment::SimulatorEnvironment(std::string initial_content)
    : impl_(std::make_unique<Impl>()) {
    impl_->content = std::move(initial_content);
}

SimulatorEnvironment::~SimulatorEnvironment() = default;
SimulatorEnvironment::SimulatorEnvironment(SimulatorEnvironment &&) noexcept = default;
SimulatorEnvironment &SimulatorEnvironment::operator=(SimulatorEnvironment &&) noexcept = default;

Observation SimulatorEnvironment::observe() {
    std::lock_guard lock(impl_->mutex);
    return Observation{++impl_->observation_sequence, impl_->content};
}

ExecutionReceipt SimulatorEnvironment::execute(const InputSequence &input) {
    std::lock_guard lock(impl_->mutex);
    impl_->actions.insert(impl_->actions.end(), input.begin(), input.end());
    impl_->was_interrupted = false;
    return ExecutionReceipt{++impl_->execution_sequence, true, "simulator input accepted"};
}

void SimulatorEnvironment::interrupt() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->was_interrupted = true;
}

void SimulatorEnvironment::set_content(std::string content) {
    std::lock_guard lock(impl_->mutex);
    impl_->content = std::move(content);
}

std::vector<InputEvent> SimulatorEnvironment::action_history() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->actions;
}

bool SimulatorEnvironment::interrupted() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->was_interrupted;
}

} // namespace mira::adapters::simulator
