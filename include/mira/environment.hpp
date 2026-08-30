#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mira {

struct Observation final {
    std::uint64_t sequence = 0;
    std::string content;
};

struct InputEvent final {
    std::string kind;
    std::string payload;
};

using InputSequence = std::vector<InputEvent>;

struct ExecutionReceipt final {
    std::uint64_t sequence = 0;
    bool accepted = false;
    std::string safe_message;
};

class IEnvironment {
public:
    virtual ~IEnvironment() = default;

    virtual Observation observe() = 0;
    virtual ExecutionReceipt execute(const InputSequence &input) = 0;
    virtual void interrupt() noexcept = 0;
};

} // namespace mira
