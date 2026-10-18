#include "Engine/Source/BlizzardCore/Source/System/URL/URL.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <curl/curl.h>

// Deterministic failure seams: all real transfers still target the fixture.
std::atomic<bool> injectDns{false}, failLaunch{false};
extern "C" CURLMsg* __real_curl_multi_info_read(CURLM*, int*);
extern "C" CURLMsg* __wrap_curl_multi_info_read(CURLM* multi, int* remaining) {
    CURLMsg* message = __real_curl_multi_info_read(multi, remaining);
    if (message && message->msg == CURLMSG_DONE && injectDns.exchange(false))
        message->data.result = CURLE_COULDNT_RESOLVE_HOST;
    return message;
}
extern "C" bool __real__ZN8Blizzard6Thread14CreateDetachedEPFjPvES1_PKc(uint32_t (*)(void*), void*, const char*);
extern "C" bool __wrap__ZN8Blizzard6Thread14CreateDetachedEPFjPvES1_PKc(uint32_t (*function)(void*), void* param, const char* name) {
    if (failLaunch.exchange(false)) return false;
    return __real__ZN8Blizzard6Thread14CreateDetachedEPFjPvES1_PKc(function, param, name);
}

namespace {
using CB = Blizzard::URL::DownloadCallback;
unsigned failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
struct State {
    std::mutex mutex;
    std::condition_variable changed;
    int complete = 0, connected = 0, code = 0;
    int64_t length = -1;
    CB::Status status = CB::DOWNLOAD_UNKNOWN_ERROR;
    std::string data, modified, order;
};
struct Callback : CB {
    std::shared_ptr<State> state = std::make_shared<State>();
    bool blocking = true, want = true, abort = false, selfDelete = false;
    std::atomic<bool> stopped{false};
    int timeout = 1000;
    ByteRange range{-1, -1};
    std::string post, type = "application/test", conditional, name, value;
    bool hasPost = false;
    bool ShouldBlock() override { return blocking; }
    bool WantData() override { return want; }
    int32_t GetTimeout() override { return timeout; }
    ByteRange GetByteRange() override { return range; }
    bool Stop() override { return stopped.load(); }
    const char* GetPostData() override { return hasPost ? post.c_str() : nullptr; }
    const char* GetPostContentType() override { return type.c_str(); }
    const char* WantIfModifiedSince() override { return conditional.empty() ? nullptr : conditional.c_str(); }
    Cookie GetCookie() override { return {name.c_str(), value.c_str()}; }
    void ConnectedOK() override { ++state->connected; state->order += 'C'; }
    void GetHTTPStatusCode(int32_t code) override { state->code = code; state->order += 'H'; }
    void GetContentLength(int64_t length) override { state->length = length; }
    void GetLastModified(const char* text) override { state->modified = text; }
    bool ReceivedData(const void* data, int32_t size) override {
        state->data.append(static_cast<const char*>(data), size); state->order += 'D'; return !abort;
    }
    void Completed(Status status) override {
        auto s = state;
        bool destroy = selfDelete;
        { std::lock_guard<std::mutex> lock(s->mutex); ++s->complete; s->status = status; s->order += 'X'; }
        if (destroy) delete this;
        s->changed.notify_all();
    }
};
bool Wait(const std::shared_ptr<State>& state) {
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->changed.wait_for(lock, std::chrono::seconds(5), [&] { return state->complete != 0; });
}
void Run(const std::string& url, Callback& cb, CB::Status status) {
    Check(Blizzard::URL::DownloadURL(url.c_str(), &cb), "valid request accepted independent of HTTP result");
    Check(Wait(cb.state), "completion bounded");
    if (cb.state->status != status) std::fprintf(stderr, "URL %s expected %d got %d\n", url.c_str(), status, cb.state->status);
    Check(cb.state->complete == 1 && cb.state->status == status, "exactly once expected completion status");
    Check(!cb.state->order.empty() && cb.state->order.back() == 'X', "Completed last");
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--connect-proxy") {
        for (bool blocking : {true, false}) {
            Callback cb; cb.blocking = blocking;
            Run("https://127.0.0.1:1/", cb, CB::DOWNLOAD_FAILED);
            Check(cb.state->connected == 0 && cb.state->code == 0, "CONNECT response is not origin connection/status");
            Check(cb.state->length == -1 && cb.state->modified.empty() && cb.state->data.empty(), "CONNECT metadata/body not delivered as origin response");
            Check(cb.state->order == "X", "TLS failure after CONNECT delivers only Completed");
        }
        System_Thread::ShutdownThreadSystem();
        std::printf("BlizzardUrlTest CONNECT proxy: %u failures\n", failures);
        return failures ? 1 : 0;
    }
    if (argc != 4) return 2;
    const std::string base = std::string("http://127.0.0.1:") + argv[1];
    const std::string other = std::string("http://127.0.0.2:") + argv[3];
    const std::string data = "0123456789abcdef";
    { Callback cb; Run(base + "/full", cb, CB::DOWNLOAD_SUCCESS);
      Check(cb.state->data == data && cb.state->length == 16 && cb.state->code == 200, "GET metadata/body");
      Check(cb.state->connected == 1 && cb.state->order.front() == 'C', "ConnectedOK once before metadata/data");
      Check(cb.state->modified == "Wed, 21 Oct 2015 07:28:00 GMT", "Last-Modified"); }
    { Callback cb; Run(base + "/chunks", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data == data && cb.state->length == -1, "chunked body"); }
    for (auto range : {CB::ByteRange{2,5}, CB::ByteRange{10,-1}, CB::ByteRange{-1,4}}) {
        Callback cb; cb.range = range; Run(base + "/full", cb, CB::DOWNLOAD_SUCCESS);
        Check(cb.state->data == (range.m_start == 2 ? "2345" : range.m_start == 10 ? "abcdef" : "cdef"), "bounded/open/suffix ranges");
    }
    { Callback cb; cb.want = false; Run(base + "/full", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.empty() && cb.state->length == 16, "HEAD"); }
    { Callback cb; cb.hasPost = true; cb.post = "body=hello"; Run(base + "/post", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data == "body=hello|application/test", "POST body/type"); }
    { Callback cb; cb.conditional = "Wed, 21 Oct 2015 07:28:00 GMT"; Run(base + "/conditional", cb, CB::DOWNLOAD_NOT_MODIFIED); Check(cb.state->code == 304 && cb.state->data.empty(), "conditional status"); }
    for (auto pair : {std::make_pair(400, CB::DOWNLOAD_BAD_REQUEST), {403, CB::DOWNLOAD_FORBIDDEN}, {404, CB::DOWNLOAD_NOT_FOUND}, {500, CB::DOWNLOAD_FAILED}}) {
        Callback cb; Run(base + "/status/" + std::to_string(pair.first), cb, pair.second); Check(cb.state->code == pair.first, "HTTP failure code");
    }
    { Callback cb; Run(base + "/redirect", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data == data && cb.state->code == 200 && cb.state->connected == 1, "redirect final metadata"); }
    { Callback cb; Run(base + "/unsafe", cb, CB::DOWNLOAD_FAILED); }
    { Callback cb; Run(std::string("http://127.0.0.1:") + argv[2] + "/", cb, CB::DOWNLOAD_CANNOT_CONNECT); Check(cb.state->connected == 0, "refused is not ConnectedOK"); }
    { Callback cb; cb.timeout = 100; Run(base + "/stall", cb, CB::DOWNLOAD_TIMEOUT); }
    { Callback cb; cb.timeout = 100; Run(base + "/active", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data == data + "xxxx", "active stream exceeds total timeout"); }
    { Callback cb; cb.timeout = 0; Run(base + "/stall", cb, CB::DOWNLOAD_SUCCESS); }
    { Callback cb; cb.abort = true; Run(base + "/chunks", cb, CB::DOWNLOAD_ABORTED); }
    { Callback cb; cb.stopped = true; Run(base + "/full", cb, CB::DOWNLOAD_STOPPED); }
    { Callback cb; cb.blocking = false; Check(Blizzard::URL::DownloadURL((base + "/stall").c_str(), &cb), "async accepted");
      std::this_thread::sleep_for(std::chrono::milliseconds(30)); auto before = std::chrono::steady_clock::now(); cb.stopped = true;
      Check(Wait(cb.state) && cb.state->status == CB::DOWNLOAD_STOPPED, "async Stop during idle");
      Check(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(200), "idle Stop promptly polled"); }
    { Callback cb; injectDns = true; Run(base + "/full", cb, CB::DOWNLOAD_NAME_NOT_RESOLVED); }
    { Callback cb; cb.blocking = false; failLaunch = true;
      Check(!Blizzard::URL::DownloadURL((base + "/full").c_str(), &cb) && cb.state->complete == 0, "SDK launch failure returns false without Completed"); }
    // Setup must accept case-insensitive cookie domains before the injected
    // launch failure. No transfer or DNS lookup is allowed for these names.
    for (const auto& input : {std::make_pair("http://Example.Test/", "yes; Domain=.example.test; Path=/"),
                              std::make_pair("http://example.test/", "yes; Domain=.EXAMPLE.TEST; Path=/"),
                              std::make_pair("http://Child.Example.Test/", "yes; Domain=.EXAMPLE.TEST; Path=/")}) {
        Callback cb; cb.blocking = false; cb.name = "casecheck"; cb.value = input.second;
        failLaunch = true;
        Check(!Blizzard::URL::DownloadURL(input.first, &cb), "case-cookie setup reaches injected launch failure");
        Check(!failLaunch.exchange(false), "case-insensitive cookie host/domain accepted before launch");
        Check(cb.state->complete == 0, "setup-only cookie case fixture never completes or transfers");
    }
    { Callback cb; cb.timeout = -1; Check(!Blizzard::URL::DownloadURL((base + "/").c_str(), &cb) && cb.state->complete == 0, "negative timeout launch failure"); }
    for (const char* url : {"file:///etc/passwd", "ftp://127.0.0.1/", "not a URL"}) {
        Callback cb; Check(!Blizzard::URL::DownloadURL(url, &cb) && cb.state->complete == 0, "invalid scheme rejected before launch");
    }
    { Callback cb; cb.name = "provided"; cb.value = "yes"; Run(base + "/cookies", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.find("provided=yes") != std::string::npos, "explicit cookie"); }
    { Callback cb; cb.name = "attributed"; cb.value = "yes; Path=/private; Secure"; Run(other + "/private/cookies", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.find("attributed=") == std::string::npos, "provided cookie secure attributes preserved"); }
    { Callback cb; Run(base + "/set/persist", cb, CB::DOWNLOAD_SUCCESS); }
    { Callback cb; Run(base + "/cookies", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.find("persist=value") != std::string::npos, "session cookie persistence"); }
    { Callback cb; Run(base + "/delete", cb, CB::DOWNLOAD_SUCCESS); }
    { Callback cb; Run(base + "/cookies", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.find("persist=") == std::string::npos, "cookie deletion"); }
    { Callback cb; Run(base + "/expired", cb, CB::DOWNLOAD_SUCCESS); }
    { Callback cb; Run(base + "/cookies", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.find("expired=") == std::string::npos, "expired cookie excluded"); }
    { Callback cb; Run(base + "/set/persist", cb, CB::DOWNLOAD_SUCCESS); }
    { Callback cb; cb.blocking = false;
      Check(Blizzard::URL::DownloadURL((base + "/slowdelete").c_str(), &cb), "concurrent deletion accepted");
      Callback setter; Run(base + "/replace", setter, CB::DOWNLOAD_SUCCESS);
      Check(Wait(cb.state), "concurrent deletion finished"); }
    { Callback cb; Run(base + "/cookies", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.find("persist=newvalue") != std::string::npos, "snapshot deletion CAS preserves newer value"); }
    { Callback cb; Run(base + "/scoped", cb, CB::DOWNLOAD_SUCCESS); }
    { Callback cb; Run(base + "/cookies", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.find("scoped=") == std::string::npos && cb.state->data.find("alien=") == std::string::npos, "path/domain isolation"); }
    { Callback cb; Run(base + "/private/cookies", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.find("scoped=yes") != std::string::npos, "cookie matching path"); }
    { Callback cb; Run(other + "/cookies", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.empty(), "host isolation"); }
    { Callback cb; Run(other + "/scoped", cb, CB::DOWNLOAD_SUCCESS); }
    { Callback cb; Run(other + "/cookies", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.find("secret=") == std::string::npos, "secure cookie not sent over untrusted HTTP loopback"); }
    { Callback cb; cb.blocking = false;
      Check(Blizzard::URL::DownloadURL((base + "/stall/cookies").c_str(), &cb), "snapshot request accepted");
      Callback setter; Run(base + "/set/latecookie", setter, CB::DOWNLOAD_SUCCESS);
      Check(Wait(cb.state) && cb.state->data.find("latecookie=") == std::string::npos, "inflight cookie snapshot isolated"); }
    { Callback cb; Run(base + "/cookies", cb, CB::DOWNLOAD_SUCCESS); Check(cb.state->data.find("latecookie=value") != std::string::npos, "old snapshot does not remove new cookie"); }
    std::vector<std::unique_ptr<Callback>> callbacks;
    for (int i = 0; i < 12; ++i) {
        auto cb = std::make_unique<Callback>(); cb->blocking = false;
        Check(Blizzard::URL::DownloadURL((base + "/set/c" + std::to_string(i)).c_str(), cb.get()), "concurrent SDK request accepted");
        callbacks.push_back(std::move(cb));
    }
    for (const auto& cb : callbacks) Check(Wait(cb->state) && cb->state->status == CB::DOWNLOAD_SUCCESS && cb->state->complete == 1, "concurrent completion");
    { Callback cb; Run(base + "/cookies", cb, CB::DOWNLOAD_SUCCESS);
      for (int i = 0; i < 12; ++i) Check(cb.state->data.find("c" + std::to_string(i) + "=value") != std::string::npos, "no lost disjoint cookie updates"); }
    { Callback cb; cb.blocking = false; cb.hasPost = true; cb.post = "snapshot-body";
      cb.conditional = "original-condition"; cb.name = "transient"; cb.value = "original-value";
      Check(Blizzard::URL::DownloadURL((base + "/snapshot").c_str(), &cb), "async option snapshot accepted");
      cb.post.assign(1000, 'z'); cb.type = "mutated"; cb.conditional = "mutated"; cb.name = "mutated"; cb.value = "mutated";
      Check(Wait(cb.state) && cb.state->data.find("snapshot-body|application/test|original-condition|") == 0 && cb.state->data.find("transient=original-value") != std::string::npos, "body/type/conditional/cookie snapshots independent of caller storage"); }
    for (bool blocking : {true, false}) {
        auto* cb = new Callback; cb->blocking = blocking; cb->selfDelete = true; cb->hasPost = true; cb->post = "owned-body";
        auto state = cb->state;
        std::string temporary = base + "/slowpost";
        Check(Blizzard::URL::DownloadURL(temporary.c_str(), cb), "self-deleting callback accepted");
        temporary.assign(1000, 'x');

        Check(Wait(state) && state->status == CB::DOWNLOAD_SUCCESS && state->data == "owned-body|application/test", "owned transient URL/body and callback deletion");
    }
    System_Thread::ShutdownThreadSystem();
    std::printf("BlizzardUrlTest: %u failures\n", failures);
    return failures ? 1 : 0;
}
