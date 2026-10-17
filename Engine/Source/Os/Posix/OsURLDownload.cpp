#include "Engine/Source/Os/OsURLDownload.h"
#include "Engine/Source/BlizzardCore/Source/System/URL/URL.h"
#include <cerrno>
#include <new>

namespace {
class Download final : public Blizzard::URL::DownloadCallback {
    OsURLDownloadCallback callback;
    void* argument;
    int32_t timeout;
    bool cancelled = false;
public:
    Download(OsURLDownloadCallback function, void* param, int32_t milliseconds)
        : callback(function), argument(param), timeout(milliseconds > 0 ? milliseconds : 0) {}
    int32_t GetTimeout() override { return timeout; }
    bool ReceivedData(const void* data, int32_t size) override {
        if (size < 0) return false;
        if (callback && !callback(argument, static_cast<const char*>(data),
                                  static_cast<uint32_t>(size), false, HTTPError::NoError)) {
            cancelled = true;
            return false;
        }
        return true;
    }
    void Completed(Status status) override {
        HTTPError::ErrorCode error = HTTPError::Failed;
        if (status == DOWNLOAD_SUCCESS || (status == DOWNLOAD_ABORTED && cancelled)) error = HTTPError::NoError;
        else if (status == DOWNLOAD_TIMEOUT) error = HTTPError::Timeout;
        else if (status == DOWNLOAD_NOT_FOUND) error = HTTPError::NotFound;
        // The legacy API closes/releases its request before the final callback.
        // The real native URL provider likewise cleans up before Completed.
        auto function = callback;
        void* param = argument;
        delete this;
        if (function) function(param, nullptr, 0, true, error);
    }
};
}

bool OsUrlDownload::DownloadString(const char* url, OsURLDownloadCallback callback, void* param, int32_t timeout) {
    if (!url || !*url) { errno = EINVAL; return false; }
    auto* download = new (std::nothrow) Download(callback, param, timeout);
    if (!download) { errno = ENOMEM; return false; }
    if (Blizzard::URL::DownloadURL(url, download)) return true;
    // Rejected setup never transfers callback ownership or schedules completion.
    delete download;
    return false;
}

bool OsURLDownload(const char* url, OsURLDownloadCallback callback, void* param) {
    return OsUrlDownload::DownloadString(url, callback, param, 5000);
}
