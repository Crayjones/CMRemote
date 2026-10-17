#include "RdpInputQueue.h"
#include <QTest>
#include <winpr/synch.h>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace {
std::mutex gateMutex;
std::condition_variable gate;
bool resetting = false, attempting = false;
int closed = 0;
void* createEvent() { return CreateEvent(nullptr, TRUE, FALSE, nullptr); }
bool setEvent(void* event) { return SetEvent(event); }
bool resetEvent(void* event) { return ResetEvent(event); }
void closeEvent(void* event) { ++closed; CloseHandle(event); }
bool gatedReset(void* event) {
    std::unique_lock<std::mutex> lock(gateMutex);
    resetting = true; gate.notify_all();
    if (!gate.wait_for(lock, std::chrono::seconds(2), [] { return attempting; })) return false;
    return ResetEvent(event);
}
}

class InputQueueTests : public QObject {
    Q_OBJECT
private slots:
    void inputWakesIdleWorker() {
        RdpInputQueue queue;
        QVERIFY(queue.healthy());
        QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_TIMEOUT));
        std::promise<void> waiting;
        auto result = std::async(std::launch::async, [&] {
            waiting.set_value();
            return WaitForSingleObject(queue.event(), 1000);
        });
        waiting.get_future().wait();
        RdpInputQueue::Command command{0}; command.scan = 0x1e; command.down = true;
        QCOMPARE(queue.enqueue(std::move(command)), RdpInputQueue::Result::Queued);
        QCOMPARE(result.get(), DWORD(WAIT_OBJECT_0));
        const auto batch = queue.take();
        QCOMPARE(batch.commands.size(), size_t(1)); QCOMPARE(batch.commands.front().scan, quint32(0x1e));
        QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_TIMEOUT));
    }
    void everyInputKindSignalsAfterDrain() {
        RdpInputQueue queue;
        for (int type = 0; type < 3; ++type) {
            RdpInputQueue::Command command{type}; command.text = QStringLiteral("owned synthetic");
            QCOMPARE(queue.enqueue(std::move(command)), RdpInputQueue::Result::Queued);
            QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_OBJECT_0));
            QCOMPARE(queue.take().commands.front().type, type);
            QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_TIMEOUT));
        }
        QCOMPARE(queue.resize({800, 600}), RdpInputQueue::Result::Queued);
        QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_OBJECT_0));
        QCOMPARE(queue.take().size, QSize(800, 600));
        QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_TIMEOUT));
        QCOMPARE(queue.clipboard(QStringLiteral("owned synthetic")), RdpInputQueue::Result::Queued);
        QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_OBJECT_0));
        QVERIFY(queue.take().clipboardDirty);
        QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_TIMEOUT));
    }
    void clipboardPermissionChangesWakeIdleWorker() {
        RdpInputQueue queue; const ClipboardStamp stamp{1, 2, 3};
        QCOMPARE(queue.clipboardControl({true, true, stamp}), RdpInputQueue::Result::Queued);
        QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_OBJECT_0));
        QCOMPARE(queue.clipboard("owned text", stamp), RdpInputQueue::Result::Queued);
        const auto clip = queue.takeClipboard(); QVERIFY(clip.clipboardControl); QVERIFY(clip.clipboardDirty);
        QCOMPARE(clip.clipboardStamp, stamp); QCOMPARE(clip.clipboardControl->stamp, stamp);
        QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_OBJECT_0));
        QVERIFY(!queue.take().clipboardControl); QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_TIMEOUT));
        queue.clipboard("obsolete", stamp); queue.clipboardControl({false, false, {2, 2, 3}});
        const auto disabled = queue.take(); QVERIFY(disabled.clipboardControl); QVERIFY(!disabled.clipboardControl->enabled);
        QVERIFY(!disabled.clipboardDirty); QVERIFY(disabled.clipboard.isEmpty());
        QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_TIMEOUT));
        queue.stop(); QCOMPARE(queue.clipboardControl({}), RdpInputQueue::Result::Stopped);
    }
    void inputWakeCannotBeLost() {
        resetting = attempting = false;
        RdpInputQueue queue({createEvent, setEvent, gatedReset, closeEvent});
        QCOMPARE(queue.enqueue({0}), RdpInputQueue::Result::Queued);
        auto drain = std::async(std::launch::async, [&] { return queue.take(); });
        {
            std::unique_lock<std::mutex> lock(gateMutex);
            QVERIFY(gate.wait_for(lock, std::chrono::seconds(1), [] { return resetting; }));
        }
        auto producer = std::async(std::launch::async, [&] {
            { std::lock_guard<std::mutex> lock(gateMutex); attempting = true; gate.notify_all(); }
            return queue.enqueue({2});
        });
        QCOMPARE(drain.get().commands.size(), size_t(1));
        QCOMPARE(producer.get(), RdpInputQueue::Result::Queued);
        QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_OBJECT_0));
        QCOMPARE(queue.take().commands.front().type, 2);
    }
    void queuedInputPreservesOrderAndReleases() {
        RdpInputQueue queue;
        for (quint32 i = 0; i < 1024; ++i) {
            RdpInputQueue::Command command{0}; command.scan = i / 2; command.down = !(i % 2);
            QCOMPARE(queue.enqueue(std::move(command)), RdpInputQueue::Result::Queued);
        }
        QCOMPARE(queue.enqueue({0}), RdpInputQueue::Result::Overflow);
        quint32 index = 0;
        while (index < 1024) {
            const auto batch = queue.take();
            QCOMPARE(batch.commands.size(), size_t(64));
            for (const auto& command : batch.commands) {
                QCOMPARE(command.scan, index / 2); QCOMPARE(command.down, !(index % 2)); ++index;
            }
            QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(index == 1024 ? WAIT_TIMEOUT : WAIT_OBJECT_0));
        }
    }
    void coalescedCommandsWakeWithoutRetainingOldText() {
        RdpInputQueue queue;
        QCOMPARE(queue.resize({800, 600}), RdpInputQueue::Result::Queued);
        QCOMPARE(queue.resize({1024, 768}), RdpInputQueue::Result::Queued);
        QCOMPARE(queue.clipboard(QStringLiteral("owned synthetic first")), RdpInputQueue::Result::Queued);
        QCOMPARE(queue.clipboard(QStringLiteral("owned synthetic last")), RdpInputQueue::Result::Queued);
        QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_OBJECT_0));
        const auto batch = queue.take();
        QCOMPARE(batch.size, QSize(1024, 768)); QVERIFY(batch.clipboardDirty);
        QCOMPARE(batch.clipboard, QStringLiteral("owned synthetic last"));
        const auto empty = queue.take(); QVERIFY(empty.clipboard.isEmpty()); QVERIFY(!empty.clipboardDirty);
        QVERIFY(!empty.size.isValid());
    }
    void shutdownWakesAndClosesInputEvent() {
        closed = 0;
        {
            RdpInputQueue queue({createEvent, setEvent, resetEvent, closeEvent});
            auto worker = std::async(std::launch::async, [&] {
                const auto status = WaitForSingleObject(queue.event(), 1000);
                return status == WAIT_OBJECT_0 && queue.take().stopping;
            });
            queue.stop(); QVERIFY(worker.get());
            QCOMPARE(closed, 0);
            QCOMPARE(queue.enqueue({0}), RdpInputQueue::Result::Stopped);
            QCOMPARE(queue.resize({800, 600}), RdpInputQueue::Result::Stopped);
            QCOMPARE(queue.clipboard(QStringLiteral("owned synthetic")), RdpInputQueue::Result::Stopped);
            QCOMPARE(WaitForSingleObject(queue.event(), 0), DWORD(WAIT_OBJECT_0));
        }
        QCOMPARE(closed, 1);
    }
    void inputEventFailuresAreExplicit() {
        RdpInputQueue absent({[]() -> void* { return nullptr; }, setEvent, resetEvent, closeEvent});
        QVERIFY(!absent.healthy()); QCOMPARE(absent.enqueue({0}), RdpInputQueue::Result::EventFailed);
        QVERIFY(absent.take().eventFailed);
        RdpInputQueue signal({createEvent, [](void*) { return false; }, resetEvent, closeEvent});
        QCOMPARE(signal.enqueue({0}), RdpInputQueue::Result::EventFailed); QVERIFY(!signal.healthy());
        RdpInputQueue reset({createEvent, setEvent, [](void*) { return false; }, closeEvent});
        QCOMPARE(reset.enqueue({0}), RdpInputQueue::Result::Queued);
        QVERIFY(reset.take().eventFailed); QVERIFY(!reset.healthy());
    }
};
QTEST_GUILESS_MAIN(InputQueueTests)
#include "rdp_input_queue.moc"
