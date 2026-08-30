#pragma once

#include <mira/environment.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mira::test {

class FakeEnvironment final : public IEnvironment {
public:
    explicit FakeEnvironment(std::string content = {}) : content_(std::move(content)) {}

    Observation observe() override { return {++observation_sequence_, content_}; }

    ExecutionReceipt execute(const InputSequence &input) override {
        actions_.insert(actions_.end(), input.begin(), input.end());
        return {++execution_sequence_, true, "fake input accepted"};
    }

    void interrupt() noexcept override { interrupted_ = true; }

    [[nodiscard]] const std::vector<InputEvent> &actions() const noexcept { return actions_; }
    [[nodiscard]] bool interrupted() const noexcept { return interrupted_; }

private:
    std::string content_;
    std::vector<InputEvent> actions_;
    std::uint64_t observation_sequence_ = 0;
    std::uint64_t execution_sequence_ = 0;
    bool interrupted_ = false;
};

} // namespace mira::test
