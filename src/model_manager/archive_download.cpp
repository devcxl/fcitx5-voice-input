#include "archive_download.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <curl/curl.h>
#include <fcntl.h>
#include <unistd.h>

namespace fcitx::model_manager {

namespace {

/// libcurl 全局初始化（进程内一次）。线程安全由 CURL 的 once 语义保证。
void EnsureCurlGlobalInit() {
    static const bool initialized = [] {
        return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    }();
    (void)initialized;
}

/// 只接受 https；重定向目标同样校验。
bool IsAllowedUrl(std::string_view url) {
    return url.rfind("https://", 0) == 0;
}

struct Sink {
    int fd = -1;
    uint64_t written = 0;
    uint64_t limit = 0;
    bool tooLarge = false;
    bool ioError = false;
};

size_t WriteCallback(char* data, size_t size, size_t nmemb, void* userdata) {
    auto* sink = static_cast<Sink*>(userdata);
    const size_t bytes = size * nmemb;
    if (sink->limit > 0 && sink->written + bytes > sink->limit) {
        // 超出清单声明的大小：中止传输（返回短写让 curl 报错）。
        sink->tooLarge = true;
        return 0;
    }
    size_t offset = 0;
    while (offset < bytes) {
        const ssize_t n = ::write(sink->fd, data + offset, bytes - offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            sink->ioError = true;
            return 0;
        }
        offset += static_cast<size_t>(n);
    }
    sink->written += bytes;
    return bytes;
}

struct ProgressState {
    const DownloadCallbacks* callbacks = nullptr;
    bool cancelled = false;
};

int ProgressCallback(void* clientp, curl_off_t dlTotal, curl_off_t dlNow,
                     curl_off_t, curl_off_t) {
    auto* state = static_cast<ProgressState*>(clientp);
    if (!state->callbacks) return 0;
    if (state->callbacks->cancelled && state->callbacks->cancelled()) {
        state->cancelled = true;
        return 1; // 非 0 中止传输
    }
    if (state->callbacks->onProgress) {
        const uint64_t total = dlTotal > 0 ? static_cast<uint64_t>(dlTotal) : 0;
        const uint64_t done = dlNow > 0 ? static_cast<uint64_t>(dlNow) : 0;
        if (!state->callbacks->onProgress(done, total)) {
            state->cancelled = true;
            return 1;
        }
    }
    return 0;
}

} // namespace

const char* DownloadStatusName(DownloadStatus status) {
    switch (status) {
        case DownloadStatus::Ok: return "ok";
        case DownloadStatus::Cancelled: return "cancelled";
        case DownloadStatus::NetworkError: return "network_error";
        case DownloadStatus::HttpError: return "http_error";
        case DownloadStatus::TooLarge: return "too_large";
        case DownloadStatus::IoError: return "io_error";
    }
    return "io_error";
}

DownloadResult DownloadArchive(const ModelSpec& spec, const std::string& destPath,
                               const DownloadCallbacks& callbacks) {
    DownloadResult result;

    if (!IsAllowedUrl(spec.archiveUrl)) {
        result.status = DownloadStatus::NetworkError;
        result.detail = "归档 URL 不是 https";
        return result;
    }
    if (destPath.empty()) {
        result.status = DownloadStatus::IoError;
        result.detail = "目标路径为空";
        return result;
    }

    EnsureCurlGlobalInit();

    // 截断写：暂存文件由安装器独占，覆盖是预期的。
    const int fd = ::open(destPath.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                          0600);
    if (fd < 0) {
        result.status = DownloadStatus::IoError;
        result.detail = std::string("无法创建下载文件: ") + std::strerror(errno);
        return result;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        ::close(fd);
        result.status = DownloadStatus::NetworkError;
        result.detail = "libcurl 初始化失败";
        return result;
    }

    Sink sink;
    sink.fd = fd;
    sink.limit = spec.archiveSize; // 硬上限：清单声明的归档大小
    ProgressState progress;
    progress.callbacks = &callbacks;

    curl_easy_setopt(curl, CURLOPT_URL, spec.archiveUrl);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // 限制重定向次数，并经 protocol 限制只允许 https。
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    // 低速阈值：避免无进展的连接永久占用。
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "voice-input-model-installer/1");
    // 不发送无关的鉴权/凭据。
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_NONE);

    const CURLcode rc = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    const bool fsyncOk = ::fsync(fd) == 0;
    const int closeRc = ::close(fd);
    curl_easy_cleanup(curl);

    result.bytes = sink.written;

    if (sink.tooLarge) {
        result.status = DownloadStatus::TooLarge;
        result.detail = "下载内容超过清单声明的大小";
        return result;
    }
    if (progress.cancelled) {
        result.status = DownloadStatus::Cancelled;
        result.detail = "下载已取消";
        return result;
    }
    if (sink.ioError) {
        result.status = DownloadStatus::IoError;
        result.detail = "写入下载文件失败";
        return result;
    }
    if (rc == CURLE_WRITE_ERROR) {
        result.status = DownloadStatus::TooLarge;
        result.detail = "下载被中止：内容超过清单声明的大小";
        return result;
    }
    if (rc != CURLE_OK) {
        result.status = DownloadStatus::NetworkError;
        result.detail = curl_easy_strerror(rc);
        return result;
    }
    if (httpCode != 200) {
        result.status = DownloadStatus::HttpError;
        result.detail = "HTTP 状态码 " + std::to_string(httpCode);
        return result;
    }
    if (!fsyncOk || closeRc != 0) {
        result.status = DownloadStatus::IoError;
        result.detail = "刷盘或关闭下载文件失败";
        return result;
    }

    result.status = DownloadStatus::Ok;
    return result;
}

} // namespace fcitx::model_manager
