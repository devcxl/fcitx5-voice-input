// 后端热切换语义的集成测试。
//
// 与 engine_ui_lifecycle_test 一样，只替换 Pipeline 的公开边界；VoiceInputEngine、
// 后台准备器、配置读写都是真实实现。本构建（未编译 sherpa-onnx）里本地后端 Init
// 会立即失败，正好用于验证「失败不回退云端」「非活跃后端不重建」等语义。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <atomic>

#include <fcitx-utils/event.h>
// 与 engine.cpp 一致：旧发行版只有弃用的 standardpath.h。
#if __has_include(<fcitx-utils/standardpaths.h>)
#include <fcitx-utils/standardpaths.h>
#define TEST_HAS_STANDARDPATHS
#else
#include <fcitx-utils/standardpath.h>
#endif
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/instance.h>

#include "asr/sherpa_offline_asr.h"
#include "engine.h"
#include "support/credential_config.h"
#include "support/xdg_sandbox.h"

namespace {

struct PipelineBoundary {
    int starts = 0;
    std::vector<std::string> installs;  // "<null>" 表示清空引擎
    bool engineInstalled = false;
    bool capturing = false;
};

PipelineBoundary boundary;

class FixtureInputContext final : public fcitx::InputContext {
public:
    explicit FixtureInputContext(fcitx::InputContextManager& manager)
        : InputContext(manager, "backend-switch-test") {}
    ~FixtureInputContext() override { destroy(); }
    const char* frontend() const override { return "backend-switch-test"; }
    std::vector<std::string> commits;

protected:
    void commitStringImpl(const std::string& text) override {
        commits.push_back(text);
    }
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent&) override {}
    void updatePreeditImpl() override {}
};

std::string StatusOf(fcitx::VoiceInputEngine& engine,
                     const fcitx::InputMethodEntry& entry,
                     fcitx::InputContext& context) {
    return engine.subModeLabelImpl(entry, context);
}

std::string ModelField(fcitx::VoiceInputEngine& engine, const char* path,
                       const char* field) {
    const fcitx::Configuration* config = engine.getSubConfig(path);
    if (!config) return "<null>";
    fcitx::RawConfig raw;
    config->save(raw);
    const std::string* value = raw.valueByPath(field);
    return value ? *value : "<empty>";
}

std::string ModelState(fcitx::VoiceInputEngine& engine, const char* path) {
    return ModelField(engine, path, "State");
}

// 本构建没有 sherpa-onnx 时，本地后端 Init 是恒 false 的桩实现。
// 用同名符号替换它，才能覆盖「同一后端重载失败保留旧引擎」的路径。
std::atomic<bool> g_localInitSucceeds{false};
std::atomic<int> g_localInitCalls{0};
std::atomic<int> g_localInitDelayMs{0};
std::atomic<int> g_localInitInFlight{0};
std::atomic<int> g_localInitMaxConcurrent{0};

bool InstalledOnlyNullFrom(size_t index) {
    for (size_t i = index; i < boundary.installs.size(); ++i) {
        if (boundary.installs[i] != "<null>") return false;
    }
    return true;
}

} // namespace

bool fcitx::SherpaOfflineAsrEngine::Init(const fcitx::AsrEngine::Config&) {
    const int inFlight = g_localInitInFlight.fetch_add(1) + 1;
    int previous = g_localInitMaxConcurrent.load();
    while (inFlight > previous &&
           !g_localInitMaxConcurrent.compare_exchange_weak(previous, inFlight)) {
    }
    const int delay = g_localInitDelayMs.load();
    if (delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    g_localInitCalls.fetch_add(1);
    g_localInitInFlight.fetch_sub(1);
    return g_localInitSucceeds.load();
}


void fcitx::Pipeline::Start() {
    ++boundary.starts;
    if (!boundary.engineInstalled) return;  // 与真实实现一致：无引擎直接返回
    boundary.capturing = true;
    running_ = true;
}
void fcitx::Pipeline::Pause() { running_ = false; }
void fcitx::Pipeline::Stop() {
    boundary.capturing = false;
    running_ = false;
}
bool fcitx::Pipeline::IsCapturing() const { return boundary.capturing; }
void fcitx::Pipeline::SetResultCallback(ResultCallback) {}
void fcitx::Pipeline::SetVadStatusCallback(VADWorker::VadStatusCallback) {}
void fcitx::Pipeline::SetAsrEngine(std::unique_ptr<fcitx::AsrEngine> engine) {
    boundary.engineInstalled = engine != nullptr;
    boundary.installs.push_back(engine ? engine->Name() : "<null>");
}

int main(int argc, char** argv) {
    // 必须在构造 fcitx::Instance 之前设置好 XDG 目录（StandardPaths 会缓存）。
    std::unique_ptr<test_support::IsolatedXdg> sandbox;
    try {
        sandbox = std::make_unique<test_support::IsolatedXdg>();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: cannot create isolated XDG sandbox: %s\n",
                     error.what());
        return 1;
    }
    const std::string scenario = argc == 2 ? argv[1] : "cloud-sync";
    char program[] = "engine-backend-switch-test";
    char disabled[] = "--disable=all";
    char* arguments[] = {program, disabled, nullptr};
    fcitx::Instance instance(2, arguments);
    instance.initialize();
    auto& manager = instance.inputContextManager();
    auto context = std::make_unique<FixtureInputContext>(manager);
    fcitx::InputMethodEntry entry("voiceinput", "Voice Input", "zh_CN",
                                  "voiceinput");

    auto engine = std::make_unique<fcitx::VoiceInputEngine>(&instance);
    int rc = -1;  // 场景结束时写入 0 / 非 0，最后统一退出事件循环
    size_t installsBeforeNotify = 0;
    std::vector<std::unique_ptr<fcitx::EventSourceTime>> timers;

    // 用绝对时间点驱动：fcitx 的 Instance 把 eventLoop.exit() 视为关机，
    // 因此整场测试只在最后退出一次。
    auto at = [&](int milliseconds, std::function<void()> fn) {
        timers.push_back(instance.eventLoop().addTimeEvent(
            CLOCK_MONOTONIC,
            fcitx::now(CLOCK_MONOTONIC) +
                static_cast<uint64_t>(milliseconds) * 1000,
            1000, [fn = std::move(fn)](fcitx::EventSourceTime*, uint64_t) {
                fn();
                return false;
            }));
    };
    auto finish = [&](int code) {
        rc = code;
        instance.eventLoop().exit();
    };
    auto fail = [&](const char* message) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        finish(1);
    };

    // 凭据权限：预置 0644 旧文件 → 保存必须收紧为 0600（含“已存在”路径）。
    const std::string credentialSecret = "test-secret-123";
    if (!test_support::SeedInsecureCredentials()) {
        return 1 + std::fprintf(stderr, "FAIL: cannot seed config fixtures\n");
    }

    // 明确起点：无论外部环境残留什么配置，本场景都从云端后端开始。
    fcitx::RawConfig initialConfig;
    initialConfig.setValueByPath("ActiveBackend", "openai");
    engine->setConfig(initialConfig);

    // 凭据写在 volcengine 子配置：当前活跃后端是 openai，不会触发重载。
    fcitx::RawConfig credentialConfigValues;
    credentialConfigValues.setValueByPath("ApiKey", credentialSecret);
    engine->setSubConfig("asr/volcengine", credentialConfigValues);

    if (const std::string error =
            test_support::VerifyCredentialPermissions(credentialSecret);
        !error.empty()) {
        return 1 + std::fprintf(stderr, "FAIL: %s\n", error.c_str());
    }
    // 已存在但被外部放宽的配置文件，再次保存仍应收紧为 0600。
    if (!test_support::RelaxCredentialPermissions()) {
        return 1 + std::fprintf(stderr, "FAIL: cannot relax credential mode\n");
    }
    engine->setSubConfig("asr/volcengine", credentialConfigValues);
    if (const std::string error =
            test_support::VerifyCredentialPermissions(credentialSecret);
        !error.empty()) {
        return 1 + std::fprintf(stderr, "FAIL: %s (after re-save)\n",
                                error.c_str());
    }

    fcitx::InputMethodActivatedEvent activated("voiceinput", context.get());
    engine->activate(entry, activated);

    if (scenario == "cloud-sync") {
        if (boundary.installs.size() != 1 || boundary.installs[0] == "<null>") {
            return 1 + std::fprintf(stderr, "FAIL: cloud not installed\n");
        }
        at(20, [&] {
            if (StatusOf(*engine, entry, *context) != "语音输入就绪") {
                fail("cloud backend never became ready");
                return;
            }
            std::printf("PASS: cloud backend installed synchronously\n");
            finish(0);
        });
        instance.eventLoop().exec();
        return rc;
    }

    if (scenario == "inactive-subconfig") {
        if (boundary.installs.size() != 1) return 1;
        at(10, [&] {
            fcitx::RawConfig sub;
            sub.setValueByPath("NumThreads", "3");
            engine->setSubConfig("asr/sherpa_offline", sub);
        });
        at(100, [&] {
            // 同一后端下只调整 VAD 参数也不应重建。
            fcitx::RawConfig sameBackend;
            sameBackend.setValueByPath("ActiveBackend", "openai");
            sameBackend.setValueByPath("VADThreshold", "33");
            engine->setConfig(sameBackend);
        });
        at(250, [&] {
            if (boundary.installs.size() != 1) {
                fail("inactive backend or VAD-only change rebuilt the engine");
                return;
            }
            std::printf("PASS: inactive backend config change did not rebuild\n");
            finish(0);
        });
        instance.eventLoop().exec();
        return rc;
    }

    if (scenario == "model-notify-inactive") {
        if (boundary.installs.size() != 1) return 1;
        const int filesBefore = test_support::CountConfigFiles();
        if (filesBefore < 1) {
            return 1 + std::fprintf(stderr,
                                    "FAIL: config measurement path is wrong\n");
        }
        at(10, [&] {
            fcitx::RawConfig note;
            engine->setSubConfig("model/offline", note);
        });
        at(250, [&] {
            if (boundary.installs.size() != 1) {
                fail("model notification rebuilt an inactive backend");
                return;
            }
            if (test_support::CountConfigFiles() != filesBefore) {
                fail("model notification wrote a config file");
                return;
            }
            std::printf("PASS: inactive model notification is a no-op\n");
            finish(0);
        });
        instance.eventLoop().exec();
        return rc;
    }

    if (scenario == "destroy-while-loading") {
        fcitx::RawConfig localConfig;
        localConfig.setValueByPath("ActiveBackend", "sherpa_offline");
        engine->setConfig(localConfig);
        if (boundary.installs.empty() || boundary.installs.back() != "<null>") {
            return 1 + std::fprintf(stderr,
                                    "FAIL: switch did not drop the cloud engine\n");
        }
        const auto start = std::chrono::steady_clock::now();
        engine.reset();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() > 200) {
            return 1 + std::fprintf(stderr,
                                    "FAIL: destruction blocked on loader\n");
        }
        std::printf("PASS: destroying the engine does not wait for the loader\n");
        return 0;
    }

    if (scenario == "reload-fail-keeps-old") {
        g_localInitSucceeds.store(true);
        at(10, [&] {
            fcitx::RawConfig localConfig;
            localConfig.setValueByPath("ActiveBackend", "sherpa_offline");
            engine->setConfig(localConfig);
        });
        at(200, [&] {
            if (boundary.installs.empty() || boundary.installs.back() == "<null>") {
                fail("injected local backend did not become ready");
                return;
            }
            if (StatusOf(*engine, entry, *context) != "语音输入就绪") {
                fail("ready status missing after local install");
                return;
            }
            // 同一后端重载失败：必须保留旧引擎，并给出明确状态。
            g_localInitSucceeds.store(false);
            fcitx::RawConfig sub;
            sub.setValueByPath("NumThreads", "4");
            engine->setSubConfig("asr/sherpa_offline", sub);
        });
        at(450, [&] {
            if (boundary.installs.empty() || boundary.installs.back() == "<null>") {
                fail("failed same-backend reload dropped the working local engine");
                return;
            }
            if (ModelState(*engine, "model/status/offline") != "failed") {
                fail("failed same-backend reload did not report failure");
                return;
            }
            if (StatusOf(*engine, entry, *context).find("继续使用") ==
                std::string::npos) {
                fail("failed same-backend reload did not explain the fallback");
                return;
            }
            std::printf("PASS: failed reload keeps the previous local engine\n");
            finish(0);
        });
        instance.eventLoop().exec();
        return rc;
    }

    if (scenario == "rapid-local-cloud-local") {
        g_localInitDelayMs.store(80);
        g_localInitSucceeds.store(true);
        auto switchTo = [&](const char* backend) {
            fcitx::RawConfig config;
            config.setValueByPath("ActiveBackend", backend);
            engine->setConfig(config);
        };
        at(10, [&] { switchTo("sherpa_offline"); });
        at(20, [&] { switchTo("openai"); });
        at(25, [&] { switchTo("sherpa_offline"); });
        at(30, [&] { switchTo("openai"); });
        at(35, [&] { switchTo("sherpa_offline"); });
        at(600, [&] {
            if (g_localInitMaxConcurrent.load() != 1) {
                fail("rapid switching ran concurrent local model loads");
                return;
            }
            if (g_localInitCalls.load() > 3) {
                fail("rapid switching accumulated local model loads");
                return;
            }
            std::printf("PASS: rapid backend switching keeps one load in flight\n");
            finish(0);
        });
        instance.eventLoop().exec();
        return rc;
    }

    if (scenario == "switch-away-status") {
        g_localInitDelayMs.store(60);
        g_localInitSucceeds.store(true);
        at(10, [&] {
            fcitx::RawConfig local;
            local.setValueByPath("ActiveBackend", "sherpa_offline");
            engine->setConfig(local);
        });
        at(40, [&] {
            // 加载仍在进行时切回云端：该后端不再是选中项。
            fcitx::RawConfig cloud;
            cloud.setValueByPath("ActiveBackend", "openai");
            engine->setConfig(cloud);
        });
        at(400, [&] {
            if (ModelState(*engine, "model/status/offline") != "idle") {
                fail("deselected local backend still reports a non-idle state");
                return;
            }
            if (!ModelField(*engine, "model/status/offline", "Version").empty()) {
                fail("deselected local backend kept a model version");
                return;
            }
            if (boundary.installs.empty() || boundary.installs.back() == "<null>") {
                fail("switch back to cloud did not install the cloud engine");
                return;
            }
            std::printf("PASS: deselected local backend reports idle\n");
            finish(0);
        });
        instance.eventLoop().exec();
        return rc;
    }

    // 其余场景：先云端就绪，再切到本地（本构建本地 Init 必失败）。
    at(10, [&] {
        if (boundary.installs.size() != 1) {
            fail("cloud backend was not installed");
            return;
        }
        fcitx::RawConfig localConfig;
        localConfig.setValueByPath("ActiveBackend", "sherpa_offline");
        engine->setConfig(localConfig);
        if (boundary.installs.size() != 2 || boundary.installs[1] != "<null>") {
            fail("cloud->local switch did not drop the old engine immediately");
            return;
        }
    });

    // 首次轮询（50ms）就可能完成失败，因此加载中状态必须在更早的时间点检查。
    at(25, [&] {
        const std::string status = StatusOf(*engine, entry, *context);
        if (status.find("加载中") == std::string::npos) {
            fail("loading status was not surfaced after the switch");
        }
    });

    at(scenario == "model-notify-active" ? 400 : 600, [&] {
        if (ModelState(*engine, "model/status/offline") != "failed") {
            fail("local backend never reported failure");
            return;
        }
        if (scenario == "switch-fail") {
            if (!InstalledOnlyNullFrom(1)) {
                fail("old cloud engine was reinstalled after local failure");
                return;
            }
            if (StatusOf(*engine, entry, *context).find("失败") ==
                std::string::npos) {
                fail("failure status was not surfaced");
                return;
            }
            if (!ModelField(*engine, "model/status/offline", "Version").empty()) {
                fail("failed local backend reported a fabricated version");
                return;
            }
            std::printf("PASS: local failure keeps no engine and never falls back\n");
            finish(0);
            return;
        }
        if (scenario == "status-readonly") {
            const int filesBefore = test_support::CountConfigFiles();
            fcitx::RawConfig write;
            write.setValueByPath("State", "ready");
            engine->setSubConfig("model/status/offline", write);
            if (ModelState(*engine, "model/status/offline") != "failed") {
                fail("status mirror accepted a write");
                return;
            }
            if (test_support::CountConfigFiles() != filesBefore) {
                fail("status mirror wrote a config file");
                return;
            }
            std::printf("PASS: model status mirror is read-only\n");
            finish(0);
            return;
        }
        if (scenario == "model-notify-active") {
            installsBeforeNotify = boundary.installs.size();
            fcitx::RawConfig note;
            engine->setSubConfig("model/offline", note);
            // 通知应重新触发准备（先摘除引擎），由后续步骤确认。
        }
    });

    if (scenario == "model-notify-active") {
        at(700, [&] {
            if (boundary.installs.size() <= installsBeforeNotify) {
                fail("active model notification did not retrigger preparation");
                return;
            }
            std::printf("PASS: active model notification reloads the backend\n");
            finish(0);
        });
    }

    instance.eventLoop().exec();
    return rc;
}
