#pragma once

// 本地 ASR 模型的异步准备器。
//
// 单独成文件的原因：线程/代次/生命周期语义自成一体，且 engine_backend.cpp 已接近
// 单文件行数上限。

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "asr/asr_engine.h"
#include "engine_backend.h"

namespace fcitx {

class EventLoop;
struct EventSourceTime;

/// 单后台准备的协调器：主线程发起请求、主线程取走结果。
///
/// 线程与生命周期边界（设计约束，勿破坏）：
///  - 工作线程只持有 shared_ptr<Job>（快照 + 结果槽）与 builder 的副本，
///    **从不捕获 VoiceInputEngine / 本对象的 this**，也不访问配置、Pipeline、
///    事件循环；因此工作线程可以在 addon 实例销毁后继续跑完并自行析构结果。
///  - 结果只由主线程的轮询定时器取走：不存在跨线程回调，也不存在
///    “schedule 到已销毁 EventDispatcher” 的窗口。
///  - 析构只做标记与定时器清理，不 join 工作线程，不阻塞主线程。
///  - 同一时刻最多一个加载线程：被取代/取消的任务仍然被跟踪并轮询，直到它
///    自己结束；新请求只替换“待执行”槽位，避免快速切换累积不可取消的加载。
///  - 前置条件：fcitx5 用 RTLD_NODELETE 加载 addon（LibraryLoadHint::DefaultHint
///    = PreventUnloadHint，见 fcitx-utils/library.h 与 sharedlibraryloader.cpp），
///    所以即使实例先销毁，工作线程执行的代码段也不会被卸载。
class AsrPrepareCoordinator {
public:
    using Builder =
        std::function<std::unique_ptr<AsrEngine>(const AsrBackendSnapshot&)>;
    /// 只在主线程、且只在本对象存活期间被调用。
    using Completion = std::function<void(uint64_t generation,
                                          std::unique_ptr<AsrEngine> engine,
                                          const std::string& error)>;

    AsrPrepareCoordinator(EventLoop& loop, Builder builder,
                          Completion completion);
    ~AsrPrepareCoordinator();

    AsrPrepareCoordinator(const AsrPrepareCoordinator&) = delete;
    AsrPrepareCoordinator& operator=(const AsrPrepareCoordinator&) = delete;

    /// 主线程：请求准备。若已有在途任务，其完成结果被丢弃，最新请求排队等待。
    /// generation 只用于标识代次，取回结果时原样返回。
    void Request(uint64_t generation, AsrBackendSnapshot snapshot);

    /// 主线程：放弃在途任务的结果并清空排队请求（不等待、不 join）。
    /// 在途线程仍被跟踪并继续轮询：单在途约束要求它结束前不得再启动新线程。
    void Cancel();

    /// 是否有请求在途或排队。
    bool IsPreparing() const { return busy_; }

private:
    struct Job {
        Job() = default;
        ~Job();
        Job(const Job&) = delete;
        Job& operator=(const Job&) = delete;

        uint64_t generation = 0;
        AsrBackendSnapshot snapshot;
        std::mutex mutex;
        std::unique_ptr<AsrEngine> engine;
        std::string error;
        bool done = false;
        bool abandoned = false;
    };
    struct Pending {
        uint64_t generation = 0;
        AsrBackendSnapshot snapshot;
    };

    void StartJob(uint64_t generation, AsrBackendSnapshot snapshot);
    void Poll();  // 主线程
    void SetPolling(bool enabled);

    EventLoop& loop_;
    Builder builder_;
    Completion completion_;

    // 主线程独占状态；Job 由工作线程共享持有（shared_ptr），故先 reset 也不会提前释放。
    std::shared_ptr<Job> job_;
    std::unique_ptr<Pending> pending_;
    std::unique_ptr<EventSourceTime> pollTimer_;
    uint64_t latestGeneration_ = 0;
    bool busy_ = false;
};

} // namespace fcitx
