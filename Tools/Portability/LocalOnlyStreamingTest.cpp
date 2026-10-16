#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "Engine/Source/BlizzardCore/Source/Packages/Streaming/Streaming.h"

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (errno=%d)\n", message, errno);
        std::exit(1);
    }
}
}

int main() {
    namespace Streaming = Blizzard::Streaming;
    const auto directory = std::filesystem::current_path();
    for (bool required : {false, true}) {
        errno = 0;
        Check(!Streaming::InitializeStreaming("unopened.mfil", "unwritten.stor", "variant", "unused", required)
              && errno == ENOTSUP, "initialization explicitly unsupported");
    }
    errno = EDOM;
    Check(!Streaming::IsStreamedFile("missing.mpq") && !Streaming::IsStreamedFile(nullptr), "no files registered as streamed");
    Check(Streaming::CheckFileRange(nullptr, 0, 0, "asset", 0) == -1, "name-only locality is unknown, not fabricated availability");
    Check(Streaming::CheckFileRange("missing.mpq", 0, 100, "asset", 0) == -1, "path locality is not managed");
    Check(errno == EDOM, "neutral locality queries preserve errno");
    errno = 0;
    Check(Streaming::GetIsLocalAmount("missing.mpq", 0, 100, "asset") == 0 && errno == ENOTSUP,
          "direct locality amount does not claim bytes exist");
    errno = 0;
    Check(!Streaming::MakeLocalAsync(nullptr, 0, 100, 0, "asset") && errno == ENOTSUP,
          "asynchronous hydration never claims queued work");
    errno = 0;
    Check(!Streaming::MakeLocalBlocking(nullptr, 0, 100, 0, "asset") && errno == ENOTSUP,
          "blocking hydration never claims downloaded data");
    errno = 0;
    Check(Streaming::ResolveFilePath("missing.mpq").empty() && errno == ENOTSUP, "no streaming path mapping fabricated");
    Streaming::Completion completion{};
    errno = 0;
    Check(!Streaming::CancelQueue(0) && errno == ENOTSUP, "uninitialized queue cancellation is safe");
    errno = 0;
    Check(!Streaming::QueueCompletion(&completion, 0) && errno == ENOTSUP, "uninitialized completion submission is safe");
    errno = 0;
    Check(!Streaming::RemoveCompletion(&completion, 0) && errno == ENOTSUP, "uninitialized completion removal is safe");
    Check(!completion.m_done && !completion.m_success && completion.m_result == 0 && !completion.m_event,
          "unsupported queue operations do not fabricate completion");
    Check(std::filesystem::current_path() == directory, "initialization does not change working directory");
    std::puts("PASS: explicit local-only streaming boundary and safe unsupported operations");
}
