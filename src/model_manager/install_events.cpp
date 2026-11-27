#include "install_events.h"

#include <cstdio>
#include <mutex>

#include <json/json.h>

namespace fcitx::model_manager {

namespace {

std::mutex g_stdoutMutex;

} // namespace

const char* InstallErrorName(InstallError error) {
    switch (error) {
        case InstallError::None: return "none";
        case InstallError::Usage: return "usage";
        case InstallError::UnknownModel: return "unknown_model";
        case InstallError::UnsafePath: return "unsafe_path";
        case InstallError::LockBusy: return "lock_busy";
        case InstallError::Cancelled: return "cancelled";
        case InstallError::DownloadFailed: return "download_failed";
        case InstallError::ArchiveMismatch: return "archive_mismatch";
        case InstallError::ArchiveUnsafe: return "archive_unsafe";
        case InstallError::ExtractFailed: return "extract_failed";
        case InstallError::FileMismatch: return "file_mismatch";
        case InstallError::MissingFile: return "missing_file";
        case InstallError::TrialLoadFailed: return "trial_load_failed";
        case InstallError::TrialLoadTimeout: return "trial_load_timeout";
        case InstallError::PublishFailed: return "publish_failed";
        case InstallError::Internal: return "internal";
    }
    return "internal";
}

const char* InstallErrorDetail(InstallError error) {
    switch (error) {
        case InstallError::None: return "安装完成";
        case InstallError::Usage: return "命令行参数不正确";
        case InstallError::UnknownModel: return "不支持该模型标识";
        case InstallError::UnsafePath: return "暂存或版本路径不安全";
        case InstallError::LockBusy: return "另一个安装进程正在使用该模型";
        case InstallError::Cancelled: return "安装已取消";
        case InstallError::DownloadFailed: return "下载失败";
        case InstallError::ArchiveMismatch: return "归档校验失败：摘要或大小不符";
        case InstallError::ArchiveUnsafe: return "归档中包含不允许的条目";
        case InstallError::ExtractFailed: return "解包失败";
        case InstallError::FileMismatch: return "文件校验失败：摘要或大小不符";
        case InstallError::MissingFile: return "归档缺少必需文件";
        case InstallError::TrialLoadFailed: return "试加载失败：模型无法被识别引擎加载";
        case InstallError::TrialLoadTimeout: return "试加载超时";
        case InstallError::PublishFailed: return "发布版本目录失败";
        case InstallError::Internal: return "内部错误";
    }
    return "内部错误";
}

const char* InstallStageName(InstallStage stage) {
    switch (stage) {
        case InstallStage::Start: return "start";
        case InstallStage::Download: return "download";
        case InstallStage::VerifyArchive: return "verify_archive";
        case InstallStage::Extract: return "extract";
        case InstallStage::VerifyFile: return "verify_file";
        case InstallStage::VerifyTrial: return "verify_trial";
        case InstallStage::Publish: return "publish";
        case InstallStage::Result: return "result";
    }
    return "start";
}

std::string ToJsonLine(const InstallEvent& event) {
    Json::Value root(Json::objectValue);
    root["event"] = InstallStageName(event.stage);

    if (!event.modelId.empty()) root["model"] = event.modelId;
    if (!event.version.empty()) root["version"] = event.version;

    switch (event.stage) {
        case InstallStage::Download:
            root["bytesDone"] = static_cast<Json::UInt64>(event.bytesDone);
            root["bytesTotal"] = static_cast<Json::UInt64>(event.bytesTotal);
            break;
        case InstallStage::VerifyFile:
        case InstallStage::Extract:
            if (!event.file.empty()) root["file"] = event.file;
            break;
        case InstallStage::Result:
            root["ok"] = event.ok;
            root["published"] = event.published;
            root["cancelled"] = event.cancelled;
            root["durabilityUnknown"] = event.durabilityUnknown;
            root["pathStateUncertain"] = event.pathStateUncertain;
            root["error"] = InstallErrorName(event.error);
            root["detail"] = event.detail.empty()
                                 ? InstallErrorDetail(event.error)
                                 : event.detail;
            root["elapsedMs"] = static_cast<Json::UInt64>(event.elapsedMs);
            if (!event.versionDir.empty()) {
                root["versionDir"] = event.versionDir;
            }
            break;
        default:
            break;
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["emitUTF8"] = true;
    std::string out = Json::writeString(builder, root);
    // writeString 会带结尾换行；这里只要内容，调用方负责换行。
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

std::string InstalledStatusJson(const ModelSpec& spec, std::string_view state) {
    Json::Value root(Json::objectValue);
    root["event"] = "installed_status";
    root["model"] = spec.id;
    root["state"] = std::string(state);
    // 以下三项同样来自内置清单（不是用户目录自报，也不来自 marker）。
    root["version"] = spec.version;
    root["archiveSize"] = static_cast<Json::UInt64>(spec.archiveSize);
    root["source"] = spec.archiveUrl;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["emitUTF8"] = true;
    std::string out = Json::writeString(builder, root);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

bool EmitInstalledStatus(const ModelSpec& spec, std::string_view state) {
    const std::string line = InstalledStatusJson(spec, state);
    std::lock_guard<std::mutex> lock(g_stdoutMutex);
    const size_t written = std::fwrite(line.data(), 1, line.size(), stdout);
    if (written != line.size()) return false;
    if (std::fputc('\n', stdout) == EOF) return false;
    return std::fflush(stdout) == 0;
}

bool EmitEvent(const InstallEvent& event) {
    const std::string line = ToJsonLine(event);
    std::lock_guard<std::mutex> lock(g_stdoutMutex);
    const size_t written = std::fwrite(line.data(), 1, line.size(), stdout);
    if (written != line.size()) return false;
    if (std::fputc('\n', stdout) == EOF) return false;
    return std::fflush(stdout) == 0;
}

} // namespace fcitx::model_manager
