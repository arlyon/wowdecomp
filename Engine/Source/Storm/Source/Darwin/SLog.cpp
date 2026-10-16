#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>
#include "Engine/Source/Os/Posix/Path.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {
struct Log : HSLOG__ {
    std::filesystem::path path;
    uint32_t flags;
    FILE* file = nullptr;
    Log* next = nullptr;
    Log(std::filesystem::path name, uint32_t options) : path(std::move(name)), flags(options) {}
    ~Log() { if (file) std::fclose(file); }
};
std::mutex s_mutex;
Log* s_logs;

Log* Find(HSLOG handle) {
    for (Log* log = s_logs; log; log = log->next) if (log == handle) return log;
    SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
    return nullptr;
}

bool Open(Log& log) {
    if (log.flags & SLOG_FLAG_NOFILE) return false;
    if (log.file) return true;
    std::error_code error;
    if (log.path.has_parent_path()) std::filesystem::create_directories(log.path.parent_path(), error);
    if (error) { SErrSetLastError(OsPosixFileError(error.value())); return false; }
    log.file = std::fopen(log.path.c_str(), log.flags & SLOG_FLAG_APPEND ? "a" : "w");
    if (!log.file) { SErrSetLastError(OsPosixFileError(errno)); return false; }
    return true;
}
}

// The mutex and empty list are initialized by C++ before use. Reinitializing
// must not discard live log handles; SLogDestroy owns their explicit teardown.
void SLogInitialize() {}

int32_t STORMAPI SLogCreate(const char* filename, uint32_t flags, HSLOG* handle) {
    if (handle) *handle = nullptr;
    if (!filename || !*filename || !handle) { SErrSetLastError(STORM_ERROR_INVALID_PARAMETER); return 0; }
    auto path = OsPosixPath(filename);
    if (!path.has_parent_path()) {
        std::error_code error;
        const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
        if (error) { SErrSetLastError(OsPosixFileError(error.value())); return 0; }
        path = executable.parent_path() / path;
    }
    auto log = new (std::nothrow) Log(std::move(path), flags);
    if (!log) { SErrSetLastError(STORM_ERROR_NOT_ENOUGH_MEMORY); return 0; }
    if ((flags & SLOG_FLAG_OPENFILE) && !(flags & SLOG_FLAG_NOFILE) && !Open(*log)) {
        delete log;
        return 0;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    log->next = s_logs;
    s_logs = log;
    *handle = log;
    return 1;
}

void STORMAPI SLogClose(HSLOG handle) {
    std::lock_guard<std::mutex> lock(s_mutex);
    for (Log** entry = &s_logs; *entry; entry = &(*entry)->next) {
        if (*entry == handle) {
            Log* log = *entry;
            *entry = log->next;
            delete log;
            return;
        }
    }
    SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
}

void STORMAPI SLogVWrite(HSLOG handle, const char* format, va_list args) {
    if (!format) { SErrSetLastError(STORM_ERROR_INVALID_PARAMETER); return; }
    std::lock_guard<std::mutex> lock(s_mutex);
    Log* log = Find(handle);
    if (!log || !Open(*log)) return;
    const auto now = std::chrono::system_clock::now();
    const time_t seconds = std::chrono::system_clock::to_time_t(now);
    tm local{};
    localtime_r(&seconds, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%m/%d %H:%M:%S", &local);
    std::fprintf(log->file, "%s ", stamp);
    // Use the client formatter, including its positional-argument option.
    char text[32768];
    SStrVPrintf(text, sizeof(text), format, args);
    const size_t length = std::strlen(text);
    bool failed = std::fwrite(text, 1, length, log->file) != length;
    if (!length || text[length - 1] != '\n') failed |= std::fputc('\n', log->file) == EOF;
    failed |= std::fflush(log->file) != 0;
    if (failed) SErrSetLastError(OsPosixFileError(errno ? errno : EIO));
}

void SLogWrite(HSLOG handle, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SLogVWrite(handle, format, args);
    va_end(args);
}

void STORMAPI SLogFlush(HSLOG handle) {
    std::lock_guard<std::mutex> lock(s_mutex);
    Log* log = Find(handle);
    if (log && log->file && std::fflush(log->file)) SErrSetLastError(OsPosixFileError(errno));
}

void STORMAPI SLogFlushAll() {
    std::lock_guard<std::mutex> lock(s_mutex);
    for (Log* log = s_logs; log; log = log->next) {
        if (log->file && std::fflush(log->file)) SErrSetLastError(OsPosixFileError(errno));
    }
}

void STORMAPI SLogDestroy() {
    std::lock_guard<std::mutex> lock(s_mutex);
    while (s_logs) {
        Log* next = s_logs->next;
        delete s_logs;
        s_logs = next;
    }
}
