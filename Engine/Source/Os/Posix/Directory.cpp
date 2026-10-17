#include <cerrno>
#include <filesystem>
#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Os/Posix/Path.h"
#include "Engine/Source/Storm/H/storm.h"

int32_t OsCreateDirectory(const char* name, int32_t recursive) {
    if (!name || !*name) {
        errno = EINVAL;
        SErrSetLastError(STORM_ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::error_code error;
    const auto path = OsPosixPath(name);
    const bool created = recursive ? std::filesystem::create_directories(path, error) :
                                     std::filesystem::create_directory(path, error);
    if (created) return 1;
    if (!error) error = std::make_error_code(std::errc::file_exists);
    errno = error.value();
    SErrSetLastError(OsPosixFileError(error.value()));
    return 0;
}
