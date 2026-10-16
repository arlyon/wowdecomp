#include "Engine/Source/Os/OsURLDownload.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}
struct Result {
    std::mutex mutex;
    std::condition_variable changed;
    std::string data;
    unsigned completions = 0;
    HTTPError::ErrorCode error = HTTPError::Failed;
    bool cancel = false;
};
bool Receive(void* argument, const char* data, uint32_t size, bool done, HTTPError::ErrorCode error) {
    auto& result = *static_cast<Result*>(argument);
    std::lock_guard<std::mutex> lock(result.mutex);
    Check(!result.completions, "no callback after completion");
    if (done) {
        Check(!data && !size, "completion does not fabricate body bytes");
        ++result.completions;
        result.error = error;
        result.changed.notify_all();
    } else {
        Check(data && error == HTTPError::NoError, "real body callback");
        result.data.append(data, size);
    }
    return !result.cancel;
}
void Run(const std::string& url, Result& result, HTTPError::ErrorCode expected, int timeout = 5000) {
    const bool accepted = timeout == 5000 ? OsURLDownload(url.c_str(), Receive, &result)
        : OsUrlDownload::DownloadString(url.c_str(), Receive, &result, timeout);
    Check(accepted, "request accepted");
    std::unique_lock<std::mutex> lock(result.mutex);
    Check(result.changed.wait_for(lock, std::chrono::seconds(8), [&] { return result.completions != 0; }), "bounded completion");
    Check(result.completions == 1 && result.error == expected, "exact completion and error mapping");
}
}
int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--connect-proxy") == 0) {
        for (unsigned i = 0; i < 2; ++i) {
            Result result;
            Run("https://127.0.0.1:1/proxy-target", result, HTTPError::Failed);
            Check(result.data.empty(), "proxy CONNECT does not fabricate origin body");
        }
        return 0;
    }
    Check(argc >= 2, "loopback fixture port supplied");
    Result rejected;
    Check(!OsURLDownload(nullptr, Receive, &rejected) && !OsURLDownload("", Receive, &rejected), "invalid setup rejected");
    Check(!rejected.completions, "rejected setup schedules no callback");
    const std::string base = std::string("http://127.0.0.1:") + argv[1];
    { Result result; Run(base + "/full", result, HTTPError::NoError); Check(result.data == "0123456789abcdef", "real legacy streamed body"); }
    { Result result; Run(base + "/status/404", result, HTTPError::NotFound); Check(result.data.empty(), "404 does not fabricate body"); }
    { Result result; Run(base + "/status/500", result, HTTPError::Failed); }
    { Result result; Run(base + "/stall", result, HTTPError::Timeout, 50); }
    { Result result; result.cancel = true; Run(base + "/chunks", result, HTTPError::NoError); Check(!result.data.empty(), "caller cancelled after real received bytes"); }
    std::puts("PASS: legacy URL adapter uses real native HTTP, completion, errors, timeout and cancellation");
}
