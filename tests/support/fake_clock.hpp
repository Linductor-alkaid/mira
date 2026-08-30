#pragma once

#include <chrono>

namespace mira::test {

class FakeClock final {
  public:
    using duration = std::chrono::steady_clock::duration;
    using time_point = std::chrono::steady_clock::time_point;

    [[nodiscard]] time_point now() const noexcept { return now_; }
    void advance(duration amount) noexcept { now_ += amount; }

  private:
    time_point now_{};
};

} // namespace mira::test
