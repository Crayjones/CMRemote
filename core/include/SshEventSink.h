#pragma once
#include <cstdint>
#include <functional>
#include <utility>

namespace rm::ssh {
// Immutable callback. It may only queue a nonblocking notification to a stable
// delivery object retained until the native worker exits and output is drained.
class EventSink final {
public:
    explicit EventSink(std::function<void(std::uint64_t)> notify) : notify_(std::move(notify)) {}
    void wake(std::uint64_t generation) const noexcept { notify_(generation); }
private:
    const std::function<void(std::uint64_t)> notify_;
};
}
