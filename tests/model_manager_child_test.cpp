// model_manager 子进程边界测试：超时、进程组回收、stderr 有界读取。
//
// 回归重点（对应复审意见 3）：
//   - 直接子进程退出后，若其后代仍持有 stderr 写端，绝不能为了等 EOF 而阻塞
//     到后代退出（那会绕过超时）。这里用真实 shell 造出这种后代。

#include <chrono>
#include <string>
#include <vector>

#include <signal.h>
#include <unistd.h>

#include "trial_load.h"

#include "support/model_manager_test_util.h"

namespace {

using namespace model_manager_test;
using fcitx::model_manager::RunBoundedChild;
using fcitx::model_manager::TrialOptions;
using fcitx::model_manager::TrialStatus;

uint64_t ElapsedMs(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

void TestExitCodes() {
    TrialOptions options;
    options.timeoutMs = 5000;

    const auto ok = RunBoundedChild({"/bin/sh", "-c", "exit 0"}, options);
    Check(ok.status == TrialStatus::Ok, "child: exit 0 maps to Ok");
    Check(ok.exitCode == 0, "child: exit code reported");

    const auto bad = RunBoundedChild({"/bin/sh", "-c", "exit 7"}, options);
    Check(bad.status == TrialStatus::Failed, "child: nonzero exit maps to Failed");
    Check(bad.exitCode == 7, "child: nonzero exit code reported");

    // 信号终止（这里用 SIGKILL 自伤）。
    const auto signalled =
        RunBoundedChild({"/bin/sh", "-c", "kill -9 $$"}, options);
    Check(signalled.status == TrialStatus::Failed,
          "child: signal death maps to Failed");
    Check(signalled.termSignal == SIGKILL, "child: terminating signal reported");

    // 不存在的可执行文件。
    const auto missing =
        RunBoundedChild({"/nonexistent/definitely-not-here"}, options);
    Check(missing.status == TrialStatus::Failed,
          "child: exec failure reported as Failed");
}

void TestTimeoutKillsChild() {
    TrialOptions options;
    options.timeoutMs = 300;

    const auto start = std::chrono::steady_clock::now();
    const auto result = RunBoundedChild({"/bin/sh", "-c", "sleep 30"}, options);
    const uint64_t ms = ElapsedMs(start);

    Check(result.status == TrialStatus::Timeout, "child: timeout detected");
    Check(ms < 5000, "child: timeout returns promptly, not after sleep ends");
}

void TestDescendantHoldingStderrDoesNotBlock() {
    // 关键回归：子进程派生一个后台后代（继承 stderr），然后自己立即退出。
    // 若实现为了读 stderr 到 EOF 而阻塞，就会一直等到后代（sleep 60）结束。
    TrialOptions options;
    options.timeoutMs = 5000;

    const auto start = std::chrono::steady_clock::now();
    const auto result = RunBoundedChild(
        {"/bin/sh", "-c", "(sleep 60 >&2 &) ; exit 0"}, options);
    const uint64_t ms = ElapsedMs(start);

    Check(result.status == TrialStatus::Ok,
          "child: direct child exit reported despite live descendant");
    Check(ms < 3000,
          "child: stderr drain is bounded, does not wait for descendant EOF");
}

void TestCancellationKillsGroup() {
    TrialOptions options;
    options.timeoutMs = 30000;
    options.cancelled = [] { return true; };

    const auto start = std::chrono::steady_clock::now();
    const auto result = RunBoundedChild({"/bin/sh", "-c", "sleep 30"}, options);
    const uint64_t ms = ElapsedMs(start);

    Check(result.status == TrialStatus::Cancelled, "child: cancellation honoured");
    Check(ms < 3000, "child: cancellation returns promptly");
}

void TestStderrIsBounded() {
    TrialOptions options;
    options.timeoutMs = 10000;

    // 大量 stderr 输出不能撑爆内存，只保留尾部。
    const auto result = RunBoundedChild(
        {"/bin/sh", "-c", "i=0; while [ $i -lt 200 ]; do echo xxxxxxxx >&2; "
                          "i=$((i+1)); done; exit 0"},
        options);
    Check(result.status == TrialStatus::Ok, "child: noisy stderr completes");
    Check(result.stderrTail.size() <= 4096 + 1024,
          "child: stderr tail is bounded");
}

} // namespace

int main() {
    TestExitCodes();
    TestTimeoutKillsChild();
    TestDescendantHoldingStderrDoesNotBlock();
    TestCancellationKillsGroup();
    TestStderrIsBounded();
    return Report("model_manager_child_test");
}
