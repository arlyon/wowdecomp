#include <cerrno>
#include "Engine/Source/BlizzardCore/Source/Packages/Streaming/Streaming.h"

// InitializeStreaming never acquires workers, transport or storage on this
// platform, so shutdown has no resources to release.
void StopStreaming() {}

namespace {
bool Unsupported() {
    errno = ENOTSUP;
    return false;
}
}

// Native Linux currently supports local assets only. Ordinary File/SFile/MPQ
// reads remain real; these entry points never install a streaming filesystem,
// start workers, write a cache, or claim that missing data was downloaded.
bool Blizzard::Streaming::InitializeStreaming(const char*, const char*, const char*, const char*, bool) {
    return Unsupported();
}

bool Blizzard::Streaming::IsStreamedFile(const char*) {
    return false;
}

int32_t Blizzard::Streaming::CheckFileRange(const char*, int64_t, int32_t, const char*, int32_t) {
    return -1; // Not managed by streaming, not a claim of local availability.
}

int32_t Blizzard::Streaming::GetIsLocalAmount(const char*, int64_t, int32_t, const char*) {
    Unsupported();
    return 0;
}

bool Blizzard::Streaming::MakeLocalAsync(Blizzard::File::StreamRecord*, int64_t, int32_t, int32_t, const char*) {
    return Unsupported();
}

bool Blizzard::Streaming::MakeLocalBlocking(Blizzard::File::StreamRecord*, int64_t, int32_t, int32_t, const char*) {
    return Unsupported();
}

std::string Blizzard::Streaming::ResolveFilePath(const std::string&) {
    Unsupported();
    return {};
}
