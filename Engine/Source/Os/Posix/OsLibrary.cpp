#include <cerrno>
#include <dlfcn.h>
#include "Engine/Source/Os/OsLibrary.h"

void* OsLoadLibrary(const char* fileName) {
    if (!fileName) {
        errno = EINVAL;
        return nullptr;
    }
    return dlopen(fileName, RTLD_NOW | RTLD_LOCAL);
}

void OsFreeLibrary(void* module) {
    if (module) {
        dlclose(module);
    }
}

void* OsGetLibraryEntryPoint(void* module, const char* name) {
    if (!module || !name) {
        errno = EINVAL;
        return nullptr;
    }
    return dlsym(module, name);
}
