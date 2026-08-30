#pragma once

#include <cstdint>

namespace mira::test {

class DeterministicIdGenerator final {
  public:
    explicit DeterministicIdGenerator(std::uint64_t next = 1) : next_(next) {}
    [[nodiscard]] std::uint64_t next() noexcept { return next_++; }

  private:
    std::uint64_t next_;
};

} // namespace mira::test
