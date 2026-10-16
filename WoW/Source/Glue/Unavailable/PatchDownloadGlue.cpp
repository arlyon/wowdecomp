#include "WoW/Source/Glue/CGlueMgr.h"
#include "Engine/Source/Storm/H/storm.h"
#include <cerrno>

#if !defined(_WIN32)
bool CGlueMgr::PatchDownloadExecuteCommand(const char*, const char*) {
    // Windows updater execution/elevation is not part of local-only native use.
    SErrSetLastError(ENOTSUP);
    errno = ENOTSUP;
    return false;
}
#endif
