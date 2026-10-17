#pragma once
#include <cstdint>
#include <functional>
#include <utility>

namespace rm::vnc {
struct GraphicsEvent;
// Immutable callbacks retained through native worker retirement. graphics is
// synchronous and worker-only: it may mutate canonical CPU state, never widgets.
// Returning false fails the transport closed. wake only queues a coalesced
// generation-tagged notification to the stable native delivery object.
class EventSink final {
public:
    EventSink(std::function<bool(const GraphicsEvent&)> graphics,
              std::function<void(std::uint64_t)> notify)
        : graphics_(std::move(graphics)), notify_(std::move(notify)) {}
    bool graphics(const GraphicsEvent& event) const noexcept {
        try { return graphics_(event); }
        catch (...) { return false; }
    }
    void wake(std::uint64_t generation) const noexcept { notify_(generation); }
private:
    const std::function<bool(const GraphicsEvent&)> graphics_;
    const std::function<void(std::uint64_t)> notify_;
};
}
