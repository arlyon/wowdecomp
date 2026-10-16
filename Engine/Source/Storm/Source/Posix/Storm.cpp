#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include <cstdlib>
#include <mutex>

namespace {
std::mutex s_lifecycleMutex;
bool s_initialized;
}
void STORMAPI StormInitialize() {
    std::lock_guard<std::mutex> lock(s_lifecycleMutex);
    if (s_initialized) return;
    SStrInitialize();
    SErrInitialize();
    SFile::Initialize();
    // SServerInitialize only resolves a Windows critical-section entry point.
    // Native locks use pthreads directly and have no corresponding loader state.
    s_initialized = true;
}
void STORMAPI StormDestroy() {
    std::lock_guard<std::mutex> lock(s_lifecycleMutex);
    if (!s_initialized) return;
    SRgnDestroy();
    SCmdDestroy();
    SFile::Destroy();
    SCompDestroy();
    SStrDestroy();
    SErrDestroy();
    SLogDestroy();
    // Native SMsg/SEvt providers are not linked; no substitute teardown is used.
    // Normal C++/libc exit owns CRT cleanup; never register the Windows _cexit loop.
    s_initialized = false;
}
void StormExit(int32_t code) {
    StormDestroy();
    std::exit(code);
}
