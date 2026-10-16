#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Os/OsVersionHash.h"
#include "Engine/Source/Storm/H/storm.h"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <new>
#include <vector>

namespace {
int32_t Fail(int error, uint32_t stormError) {
    errno = error;
    SErrSetLastError(stormError);
    return 0;
}
}

int32_t IOsVersionChecksum(IOsVersionHashCallback& callback, const char* path, int32_t loaded) {
    // The loaded Windows algorithm normalizes PE imports, relocations and writable
    // sections. Raw ELF or disk bytes are not a substitute for that byte stream.
    if (loaded) {
        return Fail(ENOTSUP, 50);
    }
    if (!path) {
        return Fail(EINVAL, 87);
    }

    HOSFILE file = OsCreateFile(path, OS_GENERIC_READ, 1, OS_OPEN_EXISTING,
        OS_FILE_ATTRIBUTE_NORMAL, OS_FILE_TYPE_ANY);
    if (file == OS_INVALID_FILE) {
        return 0;
    }

    int32_t result = 0;
    uint64_t size = OsGetFileSize(file);
    if (size > std::numeric_limits<uint32_t>::max()) {
        Fail(EFBIG, 223);
    } else {
        // Buffer the complete file before invoking the callback, as on Windows:
        // failed/short reads must not contribute a partial authentication input.
        try {
            // Grow only as reads succeed: a stale/incorrect large size must not
            // require a multi-gigabyte allocation before detecting a short read.
            std::vector<uint8_t> data;
            uint32_t offset = 0;
            bool complete = true;
            do {
                uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(size - offset, 1024 * 1024));
                data.resize(offset + chunk ? offset + chunk : 1);
                uint32_t bytesRead = 0;
                if (!OsReadFile(file, data.data() + offset, chunk, &bytesRead)) {
                    complete = false;
                    break;
                }
                if (bytesRead != chunk) {
                    Fail(EIO, 30);
                    complete = false;
                    break;
                }
                offset += chunk;
            } while (offset < size);
            if (complete) {
                callback.Process(data.data(), static_cast<uint32_t>(size));
                result = 1;
            }
        } catch (const std::bad_alloc&) {
            Fail(ENOMEM, 8);
        }
    }
    OsCloseFile(file);
    return result;
}

OsFileVersionInfo::OsFileVersionInfo(const char*) : m_fileVersion{}, m_productVersion{} {
    // Native ELF builds have no Windows VS_VERSION_INFO resource provider.
    Fail(ENOTSUP, 50);
}
