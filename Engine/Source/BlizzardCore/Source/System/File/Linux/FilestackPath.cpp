#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"

namespace {
namespace fs = std::filesystem;
using Parms = System_File::Stacked::FileParms;
bool Failure(int error, int code = 8) { errno = error; Blizzard::File::SetLastError(code); return false; }
fs::path Path(const char* name) {
    std::string text(name);
    std::replace(text.begin(), text.end(), '\\', '/');
    return fs::path(text);
}
bool ValidPath(const Parms* parms) { return parms && parms->m_path && *parms->m_path; }
bool Output(Parms& parms, const std::string& text) {
    if (!parms.m_cwdbuf || parms.m_cwdbuflen <= 0) return Failure(EINVAL);
    if (text.size() >= static_cast<size_t>(parms.m_cwdbuflen)) { *parms.m_cwdbuf = 0; return Failure(ENAMETOOLONG); }
    std::memcpy(parms.m_cwdbuf, text.c_str(), text.size() + 1);
    return true;
}
}

bool System_File::GetRootChars(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms) return Failure(EINVAL);
    parms->m_pos = parms->m_path && (*parms->m_path == '/' || *parms->m_path == '\\') ? 1 : 0;
    return true;
}

bool System_File::IsAbsolutePath(Blizzard::File::Filesystem*, Parms* parms) {
    if (!ValidPath(parms)) return Failure(EINVAL);
    return Path(parms->m_path).is_absolute();
}

bool System_File::MakeAbsolutePath(Blizzard::File::Filesystem*, Parms* parms) {
    if (!ValidPath(parms)) return Failure(EINVAL);
    std::error_code error;
    fs::path path = parms->m_makeCanonical ? fs::weakly_canonical(Path(parms->m_path), error) :
                                           fs::absolute(Path(parms->m_path), error).lexically_normal();
    if (error) return Failure(error.value());
    std::string result = path.string();
    const size_t length = std::strlen(parms->m_path);
    if (length && (parms->m_path[length - 1] == '/' || parms->m_path[length - 1] == '\\') && result.back() != '/') result += '/';
    return Output(*parms, result);
}

bool System_File::GetWorkingDirectory(Blizzard::File::Filesystem*, Parms* parms) {
    if (!parms) return Failure(EINVAL);
    std::error_code error;
    const auto path = fs::current_path(error);
    return error ? Failure(error.value()) : Output(*parms, path.string());
}

bool System_File::SetWorkingDirectory(Blizzard::File::Filesystem*, Parms* parms) {
    if (!ValidPath(parms)) return Failure(EINVAL);
    return chdir(Path(parms->m_path).c_str()) == 0 || Failure(errno);
}

bool System_File::Create(Blizzard::File::Filesystem*, Parms*) {
    // The original backend does not support creating filesystem layers via
    // this verb. Physical files are created through Open's disposition flags.
    return Failure(ENOTSUP, 9);
}

bool System_File::CreateDirectory(Blizzard::File::Filesystem*, Parms* parms) {
    if (!ValidPath(parms)) return Failure(EINVAL);
    if (parms->m_grantEveryone) return Failure(ENOTSUP);
    std::error_code error;
    const auto path = Path(parms->m_path);
    const bool created = parms->m_recursive ? fs::create_directories(path, error) : fs::create_directory(path, error);
    if (error) return Failure(error.value());
    if (created || fs::is_directory(path, error)) return true;
    return Failure(error ? error.value() : ENOTDIR);
}

bool System_File::RemoveDirectory(Blizzard::File::Filesystem*, Parms* parms) {
    if (!ValidPath(parms)) return Failure(EINVAL);
    const auto path = Path(parms->m_path);
    if (!parms->m_recursive) return rmdir(path.c_str()) == 0 || Failure(errno);
    std::error_code error;
    if (!fs::is_directory(path, error)) return Failure(error ? error.value() : ENOTDIR);
    fs::remove_all(path, error);
    return error ? Failure(error.value()) : true;
}

bool System_File::Delete(Blizzard::File::Filesystem*, Parms* parms) {
    if (!ValidPath(parms)) return Failure(EINVAL);
    return unlink(Path(parms->m_path).c_str()) == 0 || Failure(errno);
}

bool System_File::Move(Blizzard::File::Filesystem*, Parms* parms) {
    if (!ValidPath(parms) || !parms->m_newpath || !*parms->m_newpath) return Failure(EINVAL);
    // Windows MoveFile does not replace an existing destination. Preserve that
    // atomically instead of a racy exists-then-rename check.
    return renameat2(AT_FDCWD, Path(parms->m_path).c_str(), AT_FDCWD,
                     Path(parms->m_newpath).c_str(), RENAME_NOREPLACE) == 0 || Failure(errno);
}

bool System_File::Copy(Blizzard::File::Filesystem*, Parms* parms) {
    if (!ValidPath(parms) || !parms->m_newpath || !*parms->m_newpath) return Failure(EINVAL);
    std::error_code error;
    const bool copied = fs::copy_file(Path(parms->m_path), Path(parms->m_newpath),
        parms->m_overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, error);
    return copied || Failure(error ? error.value() : EIO);
}

bool System_File::GetFreeSpace(Blizzard::File::Filesystem*, Parms* parms) {
    if (!ValidPath(parms)) return Failure(EINVAL);
    std::error_code error;
    fs::path path = Path(parms->m_path);
    if (!fs::is_directory(path, error)) path = path.parent_path();
    error.clear();
    const auto info = fs::space(path.empty() ? fs::path(".") : path, error);
    if (error) return Failure(error.value());
    if (info.available > INT64_MAX) return Failure(EOVERFLOW);
    parms->m_pos = static_cast<int64_t>(info.available);
    return true;
}

bool System_File::IsReadOnly(Blizzard::File::Filesystem*, Parms* parms) {
    if (!ValidPath(parms)) return Failure(EINVAL);
    struct stat status{};
    if (stat(Path(parms->m_path).c_str(), &status) != 0) return Failure(errno);
    return !(status.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH));
}

bool System_File::ProcessDirFast(Blizzard::File::Filesystem*, Parms* parms) {
    if (!ValidPath(parms) || !parms->m_dircallback) return Failure(EINVAL);
    std::error_code error;
    fs::directory_iterator current(Path(parms->m_path), error), end;
    if (error) return Failure(error.value());
    const auto visit = [&](const fs::directory_entry& entry) {
        const std::string filename = entry.path().filename().string();
        bool directory = false;
        if (!(parms->m_mode && entry.is_symlink(error))) directory = entry.is_directory(error);
        if (error) return Failure(error.value());
        const Blizzard::File::ProcessDirParms record{parms->m_path, filename.c_str(), parms->m_cookie, directory, {}};
        return parms->m_dircallback(record);
    };
    std::vector<fs::directory_entry> entries;
    while (current != end) {
        if (parms->m_sorted) entries.push_back(*current);
        else if (!visit(*current)) return false;
        current.increment(error);
        if (error) return Failure(error.value());
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.path().filename().native() < b.path().filename().native();
    });
    for (const auto& entry : entries) if (!visit(entry)) return false;
    return true;
}

void System_File::FromNativeName(char* output, uint32_t capacity, const char* name) {
    if (!output || !capacity) return;
    if (!name) { *output = 0; errno = EINVAL; return; }
    const size_t length = std::min<size_t>(std::strlen(name), capacity - 1);
    std::memmove(output, name, length);
    output[length] = 0;
}
