#include "Engine/Source/BlizzardCore/Source/System/Version/Version.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include <cerrno>

bool Blizzard::Version::GetFileVersion(const char*, FileVersion& version) {
    // No native VS_VERSION_INFO provider: absence is not a zero product version.
    version = {};
    errno = ENOTSUP;
    Blizzard::File::SetLastError(50);
    return false;
}

char* GetComponentVersionData(uint32_t* size) {
    // BLIZZARDCOMPONENT is a Windows executable resource, not generated metadata.
    if (size) {
        *size = 0;
    }
    errno = size ? ENOTSUP : EINVAL;
    Blizzard::File::SetLastError(size ? 50 : 87);
    return nullptr;
}
