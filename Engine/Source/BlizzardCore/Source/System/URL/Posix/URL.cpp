#include "Engine/Source/BlizzardCore/Source/System/URL/URL.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <strings.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {
using Callback = Blizzard::URL::DownloadCallback;
using Clock = std::chrono::steady_clock;
using Cookies = std::map<std::string, std::string>;
std::string CookieKey(const std::string& line) {
    // Netscape cookie identity is domain, path, name (not value or expiry).
    size_t p = 0; std::string fields[7];
    for (unsigned i = 0; i < 7; ++i) {
        size_t end = line.find('\t', p);
        fields[i] = line.substr(p, end == std::string::npos ? end : end - p);
        if (end == std::string::npos && i != 6) return {};
        p = end + 1;
    }
    if (fields[0].compare(0, 10, "#HttpOnly_") == 0) fields[0].erase(0, 10);
    if (!fields[0].empty() && fields[0].front() == '.') fields[0].erase(0, 1);
    std::transform(fields[0].begin(), fields[0].end(), fields[0].begin(), [](unsigned char c) { return std::tolower(c); });
    return fields[0] + '\t' + fields[2] + '\t' + fields[5];
}
bool ExpiredCookie(const std::string& line) {
    size_t p = 0;
    for (unsigned i = 0; i < 4; ++i) { p = line.find('\t', p); if (p == std::string::npos) return true; ++p; }
    char* end = nullptr;
    long long expiry = std::strtoll(line.c_str() + p, &end, 10);
    return end == line.c_str() + p || (expiry != 0 && expiry <= std::time(nullptr));
}
struct Runtime {
    bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    std::mutex mutex;
    Cookies cookies;
    ~Runtime() { if (initialized) curl_global_cleanup(); }
};
std::shared_ptr<Runtime> GetRuntime() {
    // Each request retains this lifetime, including through detached execution.
    static auto runtime = std::make_shared<Runtime>();
    return runtime;
}
Callback::Status MapResult(CURLcode result, long code) {
    switch (result) {
        case CURLE_OK: break;
        case CURLE_OPERATION_TIMEDOUT: return Callback::DOWNLOAD_TIMEOUT;
        case CURLE_COULDNT_RESOLVE_HOST: case CURLE_COULDNT_RESOLVE_PROXY: return Callback::DOWNLOAD_NAME_NOT_RESOLVED;
        case CURLE_COULDNT_CONNECT: return Callback::DOWNLOAD_CANNOT_CONNECT;
        default: return Callback::DOWNLOAD_FAILED;
    }
    if (code >= 200 && code < 300) return Callback::DOWNLOAD_SUCCESS;
    switch (code) {
        case 304: return Callback::DOWNLOAD_NOT_MODIFIED;
        case 400: return Callback::DOWNLOAD_BAD_REQUEST;
        case 403: return Callback::DOWNLOAD_FORBIDDEN;
        case 404: return Callback::DOWNLOAD_NOT_FOUND;
        default: return Callback::DOWNLOAD_FAILED;
    }
}
struct Request {
    std::shared_ptr<Runtime> runtime;
    Callback* callback;
    CURL* easy = nullptr;
    CURLM* multi = nullptr;
    curl_slist* headers = nullptr;
    Cookies snapshot;
    std::string url, body, range, lastModified;
    int32_t timeout = 0;
    bool wantData = true, connected = false, headerDone = false, reported = false;
    long code = 0;
    Callback::Status interrupt = Callback::DOWNLOAD_SUCCESS;
    Clock::time_point activity = Clock::now();
    curl_off_t uploaded = 0;
    Request(std::shared_ptr<Runtime> r, Callback* cb) : runtime(std::move(r)), callback(cb) {}
    ~Request() {
        if (multi && easy) curl_multi_remove_handle(multi, easy);
        if (easy) curl_easy_cleanup(easy);
        if (multi) curl_multi_cleanup(multi);
        curl_slist_free_all(headers);
    }
    bool AddHeader(const std::string& value) {
        if (value.find_first_of("\r\n") != std::string::npos) return false;
        auto* next = curl_slist_append(headers, value.c_str());
        if (!next) return false;
        headers = next; return true;
    }
    bool Interrupted() {
        if (callback->Stop()) interrupt = Callback::DOWNLOAD_STOPPED;
        return interrupt != Callback::DOWNLOAD_SUCCESS;
    }
    static size_t Header(char* data, size_t size, size_t count, void* context) noexcept {
        auto& r = *static_cast<Request*>(context);
        try {
            size_t length = size * count;
            r.activity = Clock::now();
            std::string line(data, length);
            if (line.compare(0, 5, "HTTP/") == 0) {
                auto space = line.find(' ');
                r.code = space == std::string::npos ? 0 : std::strtol(line.c_str() + space + 1, nullptr, 10);
                r.headerDone = false;
                r.lastModified.clear();
                if (!r.connected) { r.connected = true; r.callback->ConnectedOK(); }
            } else if (line == "\r\n" && !r.headerDone) {
                r.headerDone = true;
                // Redirect/interim headers are not the final response metadata.
                if (r.code >= 200 && !(r.code >= 300 && r.code < 400 && r.code != 304)) {
                    r.callback->GetHTTPStatusCode(static_cast<int32_t>(r.code));
                    r.reported = true;
                    if (r.code < 300) {
                        if (!r.lastModified.empty()) r.callback->GetLastModified(r.lastModified.c_str());
                        curl_off_t bytes = -1;
                        if (curl_easy_getinfo(r.easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &bytes) == CURLE_OK && bytes >= 0)
                            r.callback->GetContentLength(bytes);
                    }
                }
            } else if (!r.headerDone && r.code >= 200 && r.code < 300 &&
                       line.size() >= 14 && strncasecmp(line.c_str(), "Last-Modified:", 14) == 0) {
                size_t start = line.find_first_not_of(" \t", 14);
                size_t end = line.find_last_not_of("\r\n \t");
                if (start != std::string::npos && end >= start) {
                    r.lastModified = line.substr(start, end - start + 1);
                }
            }
            return r.Interrupted() ? 0 : length;
        } catch (...) { r.interrupt = Callback::DOWNLOAD_FAILED; return 0; }
    }
    static size_t Write(char* data, size_t size, size_t count, void* context) noexcept {
        auto& r = *static_cast<Request*>(context);
        try {
            size_t length = size * count;
            r.activity = Clock::now();
            if (r.code >= 200 && r.code < 300 && r.wantData) {
                if (length > INT32_MAX || !r.callback->ReceivedData(data, static_cast<int32_t>(length)))
                    r.interrupt = Callback::DOWNLOAD_ABORTED;
            }
            return r.Interrupted() ? 0 : length;
        } catch (...) { r.interrupt = Callback::DOWNLOAD_FAILED; return 0; }
    }
    static int Progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t now) noexcept {
        auto& r = *static_cast<Request*>(context);
        if (now != r.uploaded) { r.uploaded = now; r.activity = Clock::now(); }
        return 0;
    }
    bool Setup(const char* address) {
        url = address;
        easy = curl_easy_init(); multi = curl_multi_init();
        if (!easy || !multi) return false;
        // Parse before accepting; curl otherwise defers malformed URLs to perform.
        std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
        if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, address, 0) != CURLUE_OK) return false;
        char *scheme = nullptr, *host = nullptr, *path = nullptr;
        if (curl_url_get(parsed.get(), CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK) return false;
        std::string protocol(scheme); curl_free(scheme);
        if (protocol != "http" && protocol != "https") return false;
        timeout = callback->GetTimeout(); if (timeout < 0) return false;
        wantData = callback->WantData();
        const char* post = callback->GetPostData();
        bool hasPost = post != nullptr;
        if (hasPost) {
            body = post;
            const char* type = callback->GetPostContentType();
            if (!AddHeader(std::string("Content-Type: ") + (type ? type : "text/html"))) return false;
        }
        const char* modified = callback->WantIfModifiedSince();
        if (modified && !AddHeader(std::string("If-Modified-Since: ") + modified)) return false;
        auto bytes = callback->GetByteRange();
        if (bytes.m_start < -1 || bytes.m_end < -1 || (bytes.m_start >= 0 && bytes.m_end >= 0 && bytes.m_end < bytes.m_start)) return false;
        if (bytes.m_start != -1 || bytes.m_end != -1)
            range = (bytes.m_start == -1 ? "" : std::to_string(bytes.m_start)) + "-" + (bytes.m_end == -1 ? "" : std::to_string(bytes.m_end));
#define SET(option, value) if (curl_easy_setopt(easy, option, value) != CURLE_OK) return false
        SET(CURLOPT_URL, url.c_str());
        SET(CURLOPT_PROTOCOLS_STR, "http,https"); SET(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        SET(CURLOPT_FOLLOWLOCATION, 1L); SET(CURLOPT_MAXREDIRS, 10L);
        SET(CURLOPT_SSL_VERIFYPEER, 1L); SET(CURLOPT_SSL_VERIFYHOST, 2L);
        SET(CURLOPT_NOSIGNAL, 1L); SET(CURLOPT_USERAGENT, "Blizzard Web Client");
        SET(CURLOPT_CONNECTTIMEOUT_MS, timeout ? static_cast<long>(timeout) : LONG_MAX);
        SET(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
        SET(CURLOPT_HEADERFUNCTION, Header); SET(CURLOPT_HEADERDATA, this);
        SET(CURLOPT_WRITEFUNCTION, Write); SET(CURLOPT_WRITEDATA, this);
        SET(CURLOPT_XFERINFOFUNCTION, Progress); SET(CURLOPT_XFERINFODATA, this); SET(CURLOPT_NOPROGRESS, 0L);
        SET(CURLOPT_COOKIEFILE, "");
        { std::lock_guard<std::mutex> lock(runtime->mutex);
          for (auto i = runtime->cookies.begin(); i != runtime->cookies.end(); )
              if (ExpiredCookie(i->second)) i = runtime->cookies.erase(i); else ++i;
          snapshot = runtime->cookies; }
        for (const auto& cookie : snapshot) { SET(CURLOPT_COOKIELIST, cookie.second.c_str()); }
        auto cookie = callback->GetCookie();
        if (cookie.m_name && *cookie.m_name && cookie.m_value) {
            if (std::strpbrk(cookie.m_name, "\r\n\t;=") || std::strpbrk(cookie.m_value, "\r\n\t")) return false;
            if (curl_url_get(parsed.get(), CURLUPART_HOST, &host, 0) != CURLUE_OK) return false;
            std::string domain(host); curl_free(host);
            if (curl_url_get(parsed.get(), CURLUPART_PATH, &path, 0) != CURLUE_OK) return false;
            std::string directory(path); curl_free(path);
            directory.resize(directory.rfind('/') + 1);
            // COOKIELIST accepts Set-Cookie syntax, including expiry/path/secure attributes.
            std::string value(cookie.m_value);
            auto split = value.find(';');
            std::string attributes = split == std::string::npos ? "" : value.substr(split);
            std::string lower = attributes;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            bool explicitDomain = lower.find("domain=") != std::string::npos;
            std::string seed = "Set-Cookie: " + std::string(cookie.m_name) + "=" + value.substr(0, split);
            if (!explicitDomain) seed += "; domain=" + domain;
            if (lower.find("path=") == std::string::npos) seed += "; path=" + directory;
            seed += attributes;
            // Parse attributes with libcurl, but do not turn a host-only WinInet
            // cookie into a domain cookie merely to use COOKIELIST's import API.
            std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> parser(curl_easy_init(), curl_easy_cleanup);
            if (!parser || curl_easy_setopt(parser.get(), CURLOPT_COOKIELIST, seed.c_str()) != CURLE_OK) return false;
            curl_slist* list = nullptr;
            if (curl_easy_getinfo(parser.get(), CURLINFO_COOKIELIST, &list) != CURLE_OK) return false;
            std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> owner(list, curl_slist_free_all);
            for (auto* p = list; p; p = p->next) {
                std::string line(p->data);
                size_t tab = line.find('\t');
                std::string target = line.substr(0, tab);
                if (target.compare(0, 10, "#HttpOnly_") == 0) target.erase(0, 10);
                if (!target.empty() && target.front() == '.') target.erase(0, 1);
                if (strcasecmp(domain.c_str(), target.c_str()) != 0 && (domain.size() <= target.size() ||
                    strcasecmp(domain.c_str() + domain.size() - target.size(), target.c_str()) != 0 ||
                    domain[domain.size() - target.size() - 1] != '.')) return false;
                if (!explicitDomain) {
                    size_t next = line.find('\t', tab + 1);
                    line.replace(tab + 1, next - tab - 1, "FALSE");
                }
                SET(CURLOPT_COOKIELIST, line.c_str());
            }
        }
        if (!range.empty()) { SET(CURLOPT_RANGE, range.c_str()); }
        if (hasPost) {
            SET(CURLOPT_POSTFIELDS, body.c_str()); SET(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        } else if (!wantData) { SET(CURLOPT_NOBODY, 1L); }
        SET(CURLOPT_HTTPHEADER, headers);
#undef SET
        return curl_multi_add_handle(multi, easy) == CURLM_OK;
    }
    void MergeCookies() {
        curl_slist* list = nullptr;
        if (curl_easy_getinfo(easy, CURLINFO_COOKIELIST, &list) != CURLE_OK) return;
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> owner(list, curl_slist_free_all);
        Cookies current;
        for (auto* p = list; p; p = p->next) { std::string line(p->data); auto key = CookieKey(line); if (!key.empty() && !ExpiredCookie(line)) current[key] = line; }
        std::lock_guard<std::mutex> lock(runtime->mutex);
        // Disjoint concurrent updates survive. Deletion uses snapshot CAS; conflicts
        // between changed values resolve in merge order, not request launch order.
        for (const auto& before : snapshot) {
            auto found = runtime->cookies.find(before.first);
            if (!current.count(before.first) && found != runtime->cookies.end() && found->second == before.second)
                runtime->cookies.erase(found);
        }
        for (const auto& after : current) {
            auto before = snapshot.find(after.first);
            if (before == snapshot.end() || before->second != after.second) runtime->cookies[after.first] = after.second;
        }
    }
    Callback::Status Perform() {
        activity = Clock::now();
        CURLcode result = CURLE_FAILED_INIT;
        curl_off_t previousReady = 0;
        for (;;) {
            if (Interrupted()) break;
            int running = 0;
            if (curl_multi_perform(multi, &running) != CURLM_OK) break;
            int remaining = 0; CURLMsg* message;
            bool done = false;
            while ((message = curl_multi_info_read(multi, &remaining))) {
                if (message->msg == CURLMSG_DONE) { result = message->data.result; done = true; }
            }
            if (done || !running) break;
            // Establishment and receiving each get their own timeout budget.
            curl_off_t ready = 0;
            if (curl_easy_getinfo(easy, CURLINFO_PRETRANSFER_TIME_T, &ready) == CURLE_OK && ready != previousReady) {
                previousReady = ready; activity = Clock::now();
            }
            if (timeout && Clock::now() - activity >= std::chrono::milliseconds(timeout)) {
                interrupt = Callback::DOWNLOAD_TIMEOUT; break;
            }
            if (curl_multi_poll(multi, nullptr, 0, 20, nullptr) != CURLM_OK) break;
        }
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
        if (code && !reported) callback->GetHTTPStatusCode(static_cast<int32_t>(code));
        MergeCookies();
        return interrupt != Callback::DOWNLOAD_SUCCESS ? interrupt : MapResult(result, code);
    }
};
uint32_t Execute(void* context) {
    std::unique_ptr<Request> request(static_cast<Request*>(context));
    Callback* callback = request->callback;
    Callback::Status status = Callback::DOWNLOAD_FAILED;
    try { status = request->Perform(); } catch (...) {}
    request.reset();
    // Completed can destroy callback, including when executing synchronously.
    callback->Completed(status);
    return 0;
}
}

bool Blizzard::URL::DownloadURL(const char* url, DownloadCallback* callback) {
    if (!url || !callback) return false;
    try {
        auto runtime = GetRuntime();
        if (!runtime->initialized) return false;
        auto request = std::make_unique<Request>(runtime, callback);
        if (!request->Setup(url)) return false;
        bool block = callback->ShouldBlock();
        if (block) { Execute(request.release()); return true; }
        // Release before launch: the worker may finish before CreateDetached returns.
        auto* raw = request.release();
        if (!Blizzard::Thread::CreateDetached(Execute, raw, "URL")) { delete raw; return false; }
        return true;
    } catch (...) { return false; }
}
