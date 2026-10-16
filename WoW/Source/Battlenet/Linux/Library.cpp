#include "WoW/Source/Battlenet/Battlenet.h"
#include "Engine/Source/Storm/H/storm.h"
#include <cerrno>

#if !defined(_WIN32)
namespace {
void UnsupportedModule() {
    SErrSetLastError(ENOTSUP);
    errno = ENOTSUP;
}
}
void* Battlenet::WindowsLibrary::GetProc(const char*) {
    UnsupportedModule();
    return nullptr;
}
void* Battlenet::WindowsLibrary::GetProc(uint32_t) {
    UnsupportedModule();
    return nullptr;
}
// Load never constructs a native instance or acquires a module handle.
Battlenet::WindowsLibrary::~WindowsLibrary() = default;

Battlenet::Library* Battlenet::Library::Load(const char* path, Allocator&) {
    // This interface loads the proprietary PE module and resolves ordinal
    // exports. The native ELF loader cannot supply that implementation.
    const int error = !path || !*path ? EINVAL : ENOTSUP;
    SErrSetLastError(error);
    errno = error;
    return nullptr;
}
#endif
