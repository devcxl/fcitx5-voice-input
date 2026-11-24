// 本地模型异步准备的单元测试。
//
// 只覆盖 AsrPrepareCoordinator 的线程/代次/生命周期语义，不加载真实模型：
// builder 是一个可控制时长的闭包，引擎是只记录析构次数的假实现。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>

#include "engine_asr_prepare.h"

using namespace std::chrono_literals;

namespace {

struct Counters {
    std::atomic<int> built{0};
    std::atomic<int> destroyed{0};
    std::atomic<int> destroyedOnMain{0};
    std::atomic<int> destroyedOffMain{0};
};

std::thread::id g_mainThread;

void NoteDestroyedThread(Counters& counters) {
    if (std::this_thread::get_id() == g_mainThread) {
        counters.destroyedOnMain.fetch_add(1);
    } else {
        counters.destroyedOffMain.fetch_add(1);
    }
}

class FakeEngine final : public fcitx::AsrEngine {
public:
    explicit FakeEngine(std::shared_ptr<Counters> counters)
        : counters_(std::move(counters)) {}
    ~FakeEngine() override {
        counters_->destroyed.fetch_add(1);
        NoteDestroyedThread(*counters_);
    }

    bool Init(const Config&) override { return true; }
    fcitx::AsrSessionStart StartSession() override {
        return {nullptr, std::nullopt};
    }
    const char* Name() const override { return "fake"; }

private:
    std::shared_ptr<Counters> counters_;
};

/// 运行事件循环直到 done() 为真或超时；返回是否满足。
bool RunUntil(fcitx::EventLoop& loop, const std::function<bool()>& done,
              int timeoutMs) {
    bool satisfied = false;
    const uint64_t deadline = fcitx::now(CLOCK_MONOTONIC) +
                              static_cast<uint64_t>(timeoutMs) * 1000;
    auto timer = loop.addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC), 1000,
        [&](fcitx::EventSourceTime* source, uint64_t) {
            if (done()) {
                satisfied = true;
                loop.exit();
                return false;
            }
            if (fcitx::now(CLOCK_MONOTONIC) >= deadline) {
                loop.exit();
                return false;
            }
            source->setNextInterval(1000);  // 1ms
            source->setEnabled(true);
            return true;
        });
    loop.exec();
    return satisfied;
}

bool SleepUntil(const std::function<bool()>& done, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

struct Completion {
    int calls = 0;
    uint64_t generation = 0;
    bool hasEngine = false;
    std::string error;
    // 持有交付的引擎，避免它在回调结束后立刻析构而干扰析构计数断言。
    std::unique_ptr<fcitx::AsrEngine> engine;
};

fcitx::AsrBackendSnapshot Snapshot() {
    fcitx::AsrBackendSnapshot snapshot;
    snapshot.backend = "sherpa_offline";
    snapshot.local = true;
    return snapshot;
}

int Fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main() {
    g_mainThread = std::this_thread::get_id();

    // 1. 异步完成：Request 立即返回，结果由主线程取走。
    {
        fcitx::EventLoop loop;
        auto counters = std::make_shared<Counters>();
        Completion completion;
        fcitx::AsrPrepareCoordinator coordinator(
            loop,
            [counters](const fcitx::AsrBackendSnapshot&) {
                std::this_thread::sleep_for(120ms);
                counters->built.fetch_add(1);
                return std::make_unique<FakeEngine>(counters);
            },
            [&](uint64_t generation, std::unique_ptr<fcitx::AsrEngine> engine,
                const std::string& error) {
                ++completion.calls;
                completion.generation = generation;
                completion.hasEngine = engine != nullptr;
                completion.error = error;
                completion.engine = std::move(engine);
            });

        const auto start = std::chrono::steady_clock::now();
        coordinator.Request(7, Snapshot());
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() > 30) return Fail("Request blocked the caller");
        if (!coordinator.IsPreparing()) return Fail("IsPreparing() false while loading");
        if (!RunUntil(loop, [&] { return completion.calls > 0; }, 3000))
            return Fail("async completion never arrived");
        if (completion.generation != 7 || !completion.hasEngine ||
            !completion.error.empty())
            return Fail("completion payload mismatch");
        if (coordinator.IsPreparing()) return Fail("IsPreparing() true after completion");
        if (counters->built != 1) return Fail("builder call count mismatch");
    }

    // 2. 初始化失败：结果为空引擎 + 非空错误。
    {
        fcitx::EventLoop loop;
        Completion completion;
        fcitx::AsrPrepareCoordinator coordinator(
            loop,
            [](const fcitx::AsrBackendSnapshot&)
                -> std::unique_ptr<fcitx::AsrEngine> { return nullptr; },
            [&](uint64_t generation, std::unique_ptr<fcitx::AsrEngine> engine,
                const std::string& error) {
                ++completion.calls;
                completion.generation = generation;
                completion.hasEngine = engine != nullptr;
                completion.error = error;
                completion.engine = std::move(engine);
            });
        coordinator.Request(3, Snapshot());
        if (!RunUntil(loop, [&] { return completion.calls > 0; }, 3000))
            return Fail("failure completion never arrived");
        if (completion.generation != 3 || completion.hasEngine ||
            completion.error != "init_failed")
            return Fail("failure payload mismatch");
    }

    // 3. 代次过滤：只有最后一次请求的结果生效，被取代的引擎由工作线程析构。
    {
        fcitx::EventLoop loop;
        auto counters = std::make_shared<Counters>();
        Completion completion;
        fcitx::AsrPrepareCoordinator coordinator(
            loop,
            [counters](const fcitx::AsrBackendSnapshot&) {
                std::this_thread::sleep_for(80ms);
                counters->built.fetch_add(1);
                return std::make_unique<FakeEngine>(counters);
            },
            [&](uint64_t generation, std::unique_ptr<fcitx::AsrEngine> engine,
                const std::string& error) {
                ++completion.calls;
                completion.generation = generation;
                completion.hasEngine = engine != nullptr;
                completion.error = error;
                completion.engine = std::move(engine);
            });

        coordinator.Request(11, Snapshot());
        coordinator.Request(12, Snapshot());
        if (!RunUntil(loop, [&] { return completion.calls > 0; }, 5000))
            return Fail("superseded request never completed");
        if (completion.calls != 1 || completion.generation != 12)
            return Fail("superseded result was applied");
        if (counters->built != 2)
            return Fail("coalescing built an unexpected number of engines");
        if (!SleepUntil([&] { return counters->destroyed == 1; }, 3000))
            return Fail("discarded engine was not destroyed off-thread");
    }

    // 4. Cancel：丢弃在途结果，不再回调。
    {
        fcitx::EventLoop loop;
        auto counters = std::make_shared<Counters>();
        int calls = 0;
        fcitx::AsrPrepareCoordinator coordinator(
            loop,
            [counters](const fcitx::AsrBackendSnapshot&) {
                std::this_thread::sleep_for(80ms);
                return std::make_unique<FakeEngine>(counters);
            },
            [&](uint64_t, std::unique_ptr<fcitx::AsrEngine>,
                const std::string&) { ++calls; });

        coordinator.Request(21, Snapshot());
        coordinator.Cancel();
        // 取消不等待：在途线程仍被跟踪（单在途约束），完成后由轮询释放。
        RunUntil(loop, [] { return false; }, 400);
        if (coordinator.IsPreparing())
            return Fail("cancelled job was never released");
        if (calls != 0) return Fail("cancelled result was delivered");
        if (!SleepUntil([&] { return counters->destroyed == 1; }, 3000))
            return Fail("cancelled engine was not destroyed off-thread");
    }

    // 5. 析构不阻塞主线程，也不让工作线程访问已销毁对象。
    {
        auto counters = std::make_shared<Counters>();
        const auto start = std::chrono::steady_clock::now();
        {
            fcitx::EventLoop loop;
            fcitx::AsrPrepareCoordinator coordinator(
                loop,
                [counters](const fcitx::AsrBackendSnapshot&) {
                    std::this_thread::sleep_for(200ms);
                    return std::make_unique<FakeEngine>(counters);
                },
                [](uint64_t, std::unique_ptr<fcitx::AsrEngine>,
                   const std::string&) {});
            coordinator.Request(31, Snapshot());
        }  // coordinator 析构：不 join
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() > 60)
            return Fail("coordinator destruction blocked on the loader");
        if (!SleepUntil([&] { return counters->destroyed == 1; }, 3000))
            return Fail("abandoned engine was not destroyed by the worker");
    }

    // 6. 排队合并：在途 + 最新一条，中间请求被跳过。
    {
        fcitx::EventLoop loop;
        auto counters = std::make_shared<Counters>();
        uint64_t lastGeneration = 0;
        int calls = 0;
        fcitx::AsrPrepareCoordinator coordinator(
            loop,
            [counters](const fcitx::AsrBackendSnapshot&) {
                std::this_thread::sleep_for(60ms);
                counters->built.fetch_add(1);
                return std::make_unique<FakeEngine>(counters);
            },
            [&](uint64_t generation, std::unique_ptr<fcitx::AsrEngine>,
                const std::string&) {
                ++calls;
                lastGeneration = generation;
            });

        coordinator.Request(41, Snapshot());
        coordinator.Request(42, Snapshot());
        coordinator.Request(43, Snapshot());
        if (!RunUntil(loop, [&] { return calls > 0; }, 5000))
            return Fail("queued request never completed");
        if (calls != 1 || lastGeneration != 43)
            return Fail("queued request did not collapse to the latest");
        if (counters->built != 2)
            return Fail("queued requests were not coalesced");
    }

    // 7. 任务已完成但尚未轮询时 Cancel：结果只能离主线程析构。
    {
        fcitx::EventLoop loop;
        auto counters = std::make_shared<Counters>();
        fcitx::AsrPrepareCoordinator coordinator(
            loop,
            [counters](const fcitx::AsrBackendSnapshot&) {
                counters->built.fetch_add(1);
                std::this_thread::sleep_for(20ms);  // 让完成早于任何轮询
                return std::make_unique<FakeEngine>(counters);
            },
            [](uint64_t, std::unique_ptr<fcitx::AsrEngine>,
               const std::string&) {});
        coordinator.Request(51, Snapshot());
        if (!SleepUntil([&] { return counters->built == 1; }, 3000))
            return Fail("builder did not run");
        std::this_thread::sleep_for(80ms);  // 任务已发布结果，但主线程从未轮询
        coordinator.Cancel();
        if (coordinator.IsPreparing())
            return Fail("Cancel left a finished job busy");
        if (!SleepUntil([&] { return counters->destroyed >= 1; }, 3000))
            return Fail("finished-but-unpolled engine was not destroyed");
        if (counters->destroyedOnMain != 0)
            return Fail("finished-but-unpolled engine was destroyed on main thread");
    }

    // 8. 任务已完成但尚未轮询时被新请求取代：旧结果离主线程析构，新结果生效。
    {
        fcitx::EventLoop loop;
        auto counters = std::make_shared<Counters>();
        int calls = 0;
        uint64_t lastGeneration = 0;
        std::unique_ptr<fcitx::AsrEngine> delivered;
        fcitx::AsrPrepareCoordinator coordinator(
            loop,
            [counters](const fcitx::AsrBackendSnapshot&) {
                counters->built.fetch_add(1);
                std::this_thread::sleep_for(20ms);
                return std::make_unique<FakeEngine>(counters);
            },
            [&](uint64_t generation, std::unique_ptr<fcitx::AsrEngine> engine,
                const std::string&) {
                ++calls;
                lastGeneration = generation;
                delivered = std::move(engine);
            });
        coordinator.Request(61, Snapshot());
        if (!SleepUntil([&] { return counters->built == 1; }, 3000))
            return Fail("builder did not run");
        std::this_thread::sleep_for(80ms);
        coordinator.Request(62, Snapshot());  // 旧任务已完成，只排队新请求
        if (!RunUntil(loop, [&] { return calls > 0; }, 5000))
            return Fail("replacement request never completed");
        if (lastGeneration != 62) return Fail("replacement generation mismatch");
        if (!SleepUntil([&] { return counters->destroyed >= 1; }, 3000))
            return Fail("superseded finished engine was not destroyed");
        if (counters->destroyedOnMain != 0)
            return Fail("superseded finished engine was destroyed on main thread");
    }

    std::printf("PASS: async ASR preparation is non-blocking, generation "
                "filtered and lifetime safe\n");
    return 0;
}
