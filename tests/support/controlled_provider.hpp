#pragma once

#include <deque>
#include <stdexcept>
#include <string>
#include <utility>

namespace mira::test {

class ControlledProvider final {
  public:
    void push(std::string value) { results_.push_back(std::move(value)); }

    [[nodiscard]] std::string invoke() {
        if (results_.empty()) {
            throw std::runtime_error("controlled provider has no result");
        }
        auto result = std::move(results_.front());
        results_.pop_front();
        return result;
    }

  private:
    std::deque<std::string> results_;
};

} // namespace mira::test
