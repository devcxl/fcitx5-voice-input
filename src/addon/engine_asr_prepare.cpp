#include "engine_asr_prepare.h"

#include <thread>
#include <utility>

#include <fcitx-utils/event.h>
#include <fcitx-utils/log.h>

namespace fcitx {

namespace {

// 本地模型准备结果的轮询间隔（微秒）。完成到上屏之间的额外延迟上限即此值。
constexpr uint64_t kPollIntervalUsec = 50000;
constexpr uint64_t kPollAccuracyUsec = 10000;

// 被丢弃的已加载引擎在独立线程析构：释放 ONNX session 不应阻塞主线程。
// 线程只持有该引擎自身，不引用 engine / Pipeline / 事件循环。
void DisposeEngineOffThread(std::unique_ptr<AsrEngine> engine) {
    if (!engine) return;
    try {
        std::thread([owned = std::move(engine)]() mutable { owned.reset(); })
            .detach();
    } catch (...) {
        // 线程创建失败时退回当前线程析构：正确性优先于延迟。
        FCITX_WARN() << "[voice-input] failed to spawn engine disposer";
    }
}

} // namespace

// -------------------------------------------------------------
// AsrPrepareCoordinator
// -------------------------------------------------------------
AsrPrepareCoordinator::AsrPrepareCoordinator(EventLoop& loop, Builder builder,
                                             Completion completion)
    : loop_(loop)
    , builder_(std::move(builder))
    , completion_(std::move(completion)) {
    pollTimer_ = loop_.addTimeEvent(
        CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + kPollIntervalUsec,
        kPollAccuracyUsec, [this](EventSourceTime* source, uint64_t) {
            Poll();
            if (!busy_) {
                source->setEnabled(false);
                return true;
            }
            // sd-event 的定时器默认 ONESHOT：重新排期后必须显式启用。
            source->setNextInterval(kPollIntervalUsec);
            source->setEnabled(true);
            return true;
        });
    pollTimer_->setEnabled(false);
}

AsrPrepareCoordinator::~AsrPrepareCoordinator() {
    Cancel();
    pollTimer_.reset();
}

// 主线程可能是 Job 的最后一个持有者（任务已完成但结果未被取走）。此时把已加载
// 的引擎交给独立线程析构，避免释放大模型阻塞主线程。
AsrPrepareCoordinator::Job::~Job() {
    if (engine) DisposeEngineOffThread(std::move(engine));
}

void AsrPrepareCoordinator::Request(uint64_t generation,
                                    AsrBackendSnapshot snapshot) {
    latestGeneration_ = generation;
    if (job_) {
        // 在途结果作废：工作线程完成时自行析构，主线程不再接收。
        {
            std::lock_guard<std::mutex> lock(job_->mutex);
            job_->abandoned = true;
        }
        pending_ = std::make_unique<Pending>(
            Pending{generation, std::move(snapshot)});
        busy_ = true;
        SetPolling(true);
        return;
    }
    StartJob(generation, std::move(snapshot));
}

void AsrPrepareCoordinator::Cancel() {
    pending_.reset();
    if (!job_) {
        busy_ = false;
        SetPolling(false);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(job_->mutex);
        job_->abandoned = true;
    }
    // 关键：不释放对在途任务的跟踪。若在这里 job_.reset()，下一次 Request 会立刻
    // 起新线程，快速本地→云→本地切换就会累积不可取消的加载线程（每个都持有模型）。
    // Poll() 会取走已完成的结果并离线程析构；未完成则继续轮询到它自己结束。
    Poll();
    if (job_) {
        busy_ = true;
        SetPolling(true);
    }
}

void AsrPrepareCoordinator::StartJob(uint64_t generation,
                                     AsrBackendSnapshot snapshot) {
    auto job = std::make_shared<Job>();
    job->generation = generation;
    job->snapshot = std::move(snapshot);
    job_ = job;
    busy_ = true;
    SetPolling(true);

    // 工作线程只捕获 job 与 builder 副本：没有 this，没有配置对象，没有事件循环。
    Builder builder = builder_;
    try {
        std::thread([job, builder = std::move(builder)]() mutable {
            std::unique_ptr<AsrEngine> engine;
            std::string error;
            try {
                engine = builder(job->snapshot);
            } catch (...) {
                error = "exception";
            }
            if (!engine && error.empty()) error = "init_failed";
            std::lock_guard<std::mutex> lock(job->mutex);
            if (!job->abandoned) {
                job->engine = std::move(engine);
                job->error = std::move(error);
            }
            // 被放弃时也要标记完成：否则协调器无法继续处理排队中的最新请求。
            // engine 留在本线程析构（unique_ptr 局部变量）。
            job->done = true;
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->error = "worker_unavailable";
        job->done = true;
    }
}

void AsrPrepareCoordinator::Poll() {
    if (!job_) return;
    auto job = job_;
    std::unique_ptr<AsrEngine> engine;
    std::string error;
    bool done = false;
    bool abandoned = false;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        done = job->done;
        abandoned = job->abandoned;
        if (done) {
            // 即使任务已被放弃也要把结果取走：否则 job_ 在主线程析构时会在这里
            // 释放已加载的大模型。
            engine = std::move(job->engine);
            error = std::move(job->error);
        }
    }
    if (!done) return;

    job_.reset();
    const uint64_t generation = job->generation;
    const bool stale = abandoned || generation != latestGeneration_;
    if (stale) {
        // 被丢弃的已加载对象不在主线程析构。
        DisposeEngineOffThread(std::move(engine));
    } else if (completion_) {
        completion_(generation, std::move(engine), error);
    }

    if (pending_) {
        auto next = std::move(pending_);
        StartJob(next->generation, std::move(next->snapshot));
        return;
    }
    if (!job_) {  // completion 可能又发起了新请求
        busy_ = false;
        SetPolling(false);
    }
}

void AsrPrepareCoordinator::SetPolling(bool enabled) {
    if (!pollTimer_) return;
    if (enabled) {
        pollTimer_->setTime(now(CLOCK_MONOTONIC) + kPollIntervalUsec);
        pollTimer_->setEnabled(true);
    } else {
        pollTimer_->setEnabled(false);
    }
}

} // namespace fcitx
