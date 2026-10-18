#include "WoW/Source/Battlenet/Battlenet.h"
#include "WoW/Source/Glue/CGlueMgr.h"
#include "Engine/Source/Storm/H/storm.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
class Allocator final : public Battlenet::Allocator {
public:
    unsigned calls = 0;
    void* Alloc(uint32_t bytes) override { ++calls; return std::malloc(bytes); }
    void Free(void* memory) override { std::free(memory); }
};
}

int main() {
    Allocator allocator;
    for (const char* path : {"Battle.net.dll", "nonexistent-native-module.so"}) {
        errno = 0;
        SErrSetLastError(0);
        Check(!Battlenet::Library::Load(path, allocator), "unsupported ordinal module must not load");
        Check(errno == ENOTSUP && SErrGetLastError() == ENOTSUP, "module failure is explicit");
    }
    for (const char* path : {static_cast<const char*>(nullptr), ""}) {
        Check(!Battlenet::Library::Load(path, allocator), "invalid module path must not load");
        Check(errno == EINVAL && SErrGetLastError() == EINVAL, "invalid module path reports invalid input");
    }
    Check(allocator.calls == 0, "unsupported loading does not allocate a fake module");
    Battlenet::WindowsLibrary unsupported{};
    Check(!unsupported.GetProc("missing") && errno == ENOTSUP,
          "named lookup cannot pretend to resolve a Windows export");
    Check(!unsupported.GetProc(uint32_t{1}) && errno == ENOTSUP,
          "ordinal lookup cannot pretend to resolve a Windows export");
    Check(!CGlueMgr::PatchDownloadExecuteCommand("must-not-run.exe", ""), "Windows updater must not launch");
    Check(errno == ENOTSUP && SErrGetLastError() == ENOTSUP, "updater failure is explicit");
    std::puts("PASS: unsupported ordinal module/updater fail explicitly; neither feature is implemented");
}
