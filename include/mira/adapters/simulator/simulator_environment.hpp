#pragma once

#include <mira/environment.hpp>

#include <memory>
#include <string>
#include <vector>

namespace mira::adapters::simulator {

class SimulatorEnvironment final : public IEnvironment {
public:
    explicit SimulatorEnvironment(std::string initial_content = {});
    ~SimulatorEnvironment() override;

    SimulatorEnvironment(const SimulatorEnvironment &) = delete;
    SimulatorEnvironment &operator=(const SimulatorEnvironment &) = delete;
    SimulatorEnvironment(SimulatorEnvironment &&) noexcept;
    SimulatorEnvironment &operator=(SimulatorEnvironment &&) noexcept;

    Observation observe() override;
    ExecutionReceipt execute(const InputSequence &input) override;
    void interrupt() noexcept override;

    void set_content(std::string content);
    [[nodiscard]] std::vector<InputEvent> action_history() const;
    [[nodiscard]] bool interrupted() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mira::adapters::simulator
