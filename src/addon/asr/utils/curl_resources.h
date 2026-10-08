#pragma once

#include <memory>
#include <string>

#include <curl/curl.h>

namespace fcitx {

struct CurlHandleDeleter {
    void operator()(CURL* handle) const noexcept { curl_easy_cleanup(handle); }
};

using CurlHandle = std::unique_ptr<CURL, CurlHandleDeleter>;

class CurlHeaders {
public:
    bool Append(const std::string& header) {
        auto* appended = curl_slist_append(headers_.get(), header.c_str());
        if (!appended) return false;
        // append returns the existing head on success; release before reset
        // so that head is not freed. Failure must retain the original list.
        headers_.release();
        headers_.reset(appended);
        return true;
    }

    curl_slist* Get() const { return headers_.get(); }

private:
    struct Deleter {
        void operator()(curl_slist* headers) const noexcept {
            curl_slist_free_all(headers);
        }
    };
    std::unique_ptr<curl_slist, Deleter> headers_;
};

} // namespace fcitx
