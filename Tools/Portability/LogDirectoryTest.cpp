#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Storm/H/storm.h"

namespace {
unsigned failures;
void Check(bool valid, const char* message) {
    if (!valid) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
}

int main() {
    char pattern[] = "native-log-test-XXXXXX";
    char* temporary = mkdtemp(pattern);
    if (!temporary) return 2;
    const auto root = std::filesystem::absolute(temporary);
    Check(!OsCreateDirectory(nullptr, 0) && SErrGetLastError() == STORM_ERROR_INVALID_PARAMETER, "invalid directory input");
    const std::string nested = root.string() + "\\one\\two";
    Check(!OsCreateDirectory(nested.c_str(), 0), "non-recursive mkdir rejects missing parent");
    Check(OsCreateDirectory(nested.c_str(), 1) && std::filesystem::is_directory(root / "one/two"), "recursive mkdir translates client path separators");
    Check(!OsCreateDirectory(nested.c_str(), 1) && SErrGetLastError() == 183, "existing directory reports legacy already-exists error");
    HSLOG log = nullptr;
    const std::string path = root.string() + "\\logs\\gx.log";
    Check(SLogCreate(path.c_str(), 0, &log) && log, "deferred log creation");
    Check(!std::filesystem::exists(root / "logs/gx.log"), "deferred open does not touch filesystem early");
    SLogWrite(log, "first %u", 42u);
    SLogFlush(log);
    Check(Read(root / "logs/gx.log").find("first 42\n") != std::string::npos, "real formatted log write and flush");
    std::vector<std::thread> writers;
    for (unsigned i = 0; i < 4; ++i) writers.emplace_back([=] {
        for (unsigned j = 0; j < 40; ++j) SLogWrite(log, "worker %u record %u", i, j);
    });
    for (auto& writer : writers) writer.join();
    SLogClose(log);
    const std::string contents = Read(root / "logs/gx.log");
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = 0; j < 40; ++j) {
        const std::string record = "worker " + std::to_string(i) + " record " + std::to_string(j) + "\n";
        Check(contents.find(record) != std::string::npos, "concurrent records remain complete");
    }
    Check(SLogCreate(path.c_str(), SLOG_FLAG_APPEND | SLOG_FLAG_OPENFILE, &log), "append log open");
    SLogWrite(log, "last");
    SLogClose(log);
    Check(Read(root / "logs/gx.log").find("first 42\n") != std::string::npos, "append preserves prior data");
    Check(SLogCreate(path.c_str(), SLOG_FLAG_OPENFILE, &log), "truncate log open");
    SLogWrite(log, "replacement");
    SLogClose(log);
    Check(Read(root / "logs/gx.log").find("first 42") == std::string::npos, "non-append open truncates old log");
    const std::string absent = (root / "no-file.log").string();
    Check(SLogCreate(absent.c_str(), SLOG_FLAG_NOFILE | SLOG_FLAG_OPENFILE, &log), "explicit no-file log handle");
    SLogWrite(log, "do not persist");
    SLogClose(log);
    Check(!std::filesystem::exists(absent), "NOFILE does not create a file");
    const std::string invalid = (root / "logs/gx.log/not-a-directory").string();
    Check(!SLogCreate(invalid.c_str(), SLOG_FLAG_OPENFILE, &log) && !log, "eager log open reports filesystem error");
    SLogWrite(nullptr, "invalid");
    Check(SErrGetLastError() == STORM_ERROR_INVALID_PARAMETER, "invalid log handles fail without dereferencing them");
    SLogDestroy();
    std::filesystem::remove_all(root);
    if (!failures) std::puts("PASS: real directories, deferred/eager logging, concurrency, append, truncate and failures");
    return failures ? 1 : 0;
}
