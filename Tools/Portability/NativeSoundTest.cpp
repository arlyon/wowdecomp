#include "fmod.hpp"
#include "fmod_dsp.h"
#include "fmod_os_misc.h"
#include "fmod_os_net.h"
#include "fmod_output_sdl.h"
#include "fmod_systemi.h"
#include <SDL.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>

static void require(bool value, const char *what)
{ if (!value) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); } }
static void ok(FMOD_RESULT value, const char *what)
{ if (value != FMOD_OK) { std::fprintf(stderr, "FAIL: %s (%d)\n", what, value); std::exit(1); } }
static std::atomic<unsigned int> mixed{0};
static FMOD_RESULT F_CALLBACK observe(FMOD_DSP_STATE *, float *in, float *out, unsigned int frames, int inch, int outch)
{
    for (unsigned int f = 0; f < frames; ++f)
        for (int c = 0; c < outch; ++c)
        {
            float v = c < inch ? in[f * inch + c] : 0;
            out[f * outch + c] = v;
            if (std::isfinite(v) && std::fabs(v) > 0.001f) mixed.fetch_add(1, std::memory_order_relaxed);
        }
    return FMOD_OK;
}
static std::vector<unsigned char> wav;
static unsigned int cursor = 0, opens = 0, reads = 0, seeks = 0, closes = 0;
static FMOD_RESULT F_CALLBACK openFixture(const char *, int, unsigned int *size, void **handle, void **userdata)
{ cursor = 0; ++opens; *size = wav.size(); *handle = &cursor; *userdata = nullptr; return FMOD_OK; }
static FMOD_RESULT F_CALLBACK closeFixture(void *handle, void *)
{ require(handle == &cursor, "callback close ownership"); ++closes; return FMOD_OK; }
static FMOD_RESULT F_CALLBACK readFixture(void *, void *buffer, unsigned int bytes, unsigned int *read, void *)
{
    ++reads; *read = bytes < wav.size() - cursor ? bytes : wav.size() - cursor;
    std::memcpy(buffer, wav.data() + cursor, *read); cursor += *read;
    return *read == bytes ? FMOD_OK : FMOD_ERR_FILE_EOF;
}
static FMOD_RESULT F_CALLBACK seekFixture(void *, unsigned int position, void *)
{ ++seeks; if (position > wav.size()) return FMOD_ERR_FILE_COULDNOTSEEK; cursor = position; return FMOD_OK; }
static void put(unsigned int offset, unsigned int value, unsigned int n)
{ for (unsigned int b = 0; b < n; ++b) wav[offset + b] = value >> (b * 8); }
static void makeFixture()
{
    const unsigned int frames = 48000;
    wav.resize(44 + frames * 2);
    std::memcpy(wav.data(), "RIFF", 4); put(4, wav.size() - 8, 4);
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8); put(16, 16, 4); put(20, 1, 2); put(22, 1, 2);
    put(24, 48000, 4); put(28, 96000, 4); put(32, 2, 2); put(34, 16, 2);
    std::memcpy(wav.data() + 36, "data", 4); put(40, frames * 2, 4);
    for (unsigned int f = 0; f < frames; ++f)
        put(44 + f * 2, static_cast<unsigned short>(static_cast<short>(12000 * std::sin(f * 440.0 * 6.283185307179586 / 48000))), 2);
}
static std::atomic<unsigned int> callbacks{0};
static FMOD_RESULT F_CALLBACK failingMixer(FMOD_OUTPUT_STATE *, void *, unsigned int)
{ callbacks.fetch_add(1); return FMOD_ERR_INTERNAL; }
static THREAD_RETURNTYPE signalThread(void *p)
{ FMOD_OS_Semaphore_Signal(static_cast<FMOD_OS_SEMAPHORE *>(p)); THREAD_RETURN }
static void platformChecks()
{
    FMOD_OS_CRITICALSECTION *crit = nullptr;
    ok(FMOD_OS_CriticalSection_Create(&crit, true), "create recursive memory mutex");
    ok(FMOD_OS_CriticalSection_Enter(crit), "mutex enter");
    ok(FMOD_OS_CriticalSection_Enter(crit), "recursive mutex enter");
    ok(FMOD_OS_CriticalSection_Leave(crit), "recursive mutex leave");
    ok(FMOD_OS_CriticalSection_Leave(crit), "mutex leave");
    ok(FMOD_OS_CriticalSection_Free(crit), "mutex free");
    FMOD_OS_SEMAPHORE *semaphore = nullptr;
    ok(FMOD_OS_Semaphore_Create(&semaphore), "create semaphore");
    void *thread = nullptr;
    ok(FMOD_OS_Thread_Create("NativeSoundTest", signalThread, semaphore, FMOD_THREAD_PRIORITY_NORMAL, nullptr, 0, &thread), "create pthread");
    ok(FMOD_OS_Semaphore_Wait(semaphore), "wait semaphore");
    ok(FMOD_OS_Thread_Destroy(thread), "join pthread");
    ok(FMOD_OS_Semaphore_Free(semaphore), "free semaphore");
    unsigned int before, after;
    ok(FMOD_OS_Time_GetMs(&before), "clock before"); ok(FMOD_OS_Time_Sleep(2), "sleep");
    ok(FMOD_OS_Time_GetMs(&after), "clock after"); require(after - before >= 1, "monotonic elapsed time");
    void *file = reinterpret_cast<void *>(1); unsigned int size = 0;
    require(FMOD_OS_File_Open("/wowdecomp-missing-audio-fixture", const_cast<char *>("rb"), 0, &size, &file) == FMOD_ERR_FILE_NOTFOUND && !file, "missing POSIX file failure");
    FILE *fixture = std::tmpfile();
    require(fixture != nullptr, "create generated temporary PCM fixture");
    const unsigned char pcm[] = {0, 1, 2, 3, 4, 5};
    require(std::fwrite(pcm, 1, sizeof(pcm), fixture) == sizeof(pcm) && !std::fflush(fixture), "write generated fixture");
    char path[64]; std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fileno(fixture));
    ok(FMOD_OS_File_Open(path, const_cast<char *>("rb"), 0, &size, &file), "open real read-only POSIX file");
    require(size == sizeof(pcm), "POSIX file size");
    unsigned char buffer[16] = {}; unsigned int read = 0;
    require(FMOD_OS_File_Read(file, buffer, sizeof(buffer), &read) == FMOD_ERR_FILE_EOF && read == sizeof(pcm), "short EOF returns actual byte count");
    require(!std::memcmp(pcm, buffer, sizeof(pcm)), "real POSIX bytes");
    ok(FMOD_OS_File_Seek(file, 0), "POSIX seek");
    ok(FMOD_OS_File_Read(file, buffer, sizeof(pcm), &read), "POSIX read after seek");
    require(read == sizeof(pcm), "full read byte count");
    ok(FMOD_OS_File_Close(file), "close POSIX read handle"); std::fclose(fixture); file = nullptr;
    require(FMOD_OS_Net_Connect("no-network-must-be-used.invalid", 80, &file) == FMOD_ERR_UNSUPPORTED && !file, "network explicitly unsupported without resolution");
    require(FMOD_OS_Net_Listen(0, &file) == FMOD_ERR_UNSUPPORTED && !file, "DSP networking explicitly unsupported");
}
static void outputFailureChecks()
{
    auto *description = FMOD::getSDLDescription();
    FMOD_OUTPUT_STATE output = {};
    output.readfrommixer = failingMixer;
    int rate = 48000;
    FMOD_SOUND_FORMAT format = FMOD_SOUND_FORMAT_PCM16;
    require(description->init(&output, 1000000, FMOD_INIT_NORMAL, &rate, 2, &format, 256, 4, nullptr) == FMOD_ERR_INVALID_PARAM && !output.plugindata, "invalid driver cleanup");
    ok(description->init(&output, 0, FMOD_INIT_NORMAL, &rate, 2, &format, 256, 4, nullptr), "open callback failure fixture");
    require(format == FMOD_SOUND_FORMAT_PCMFLOAT, "native float format negotiated");
    SDL_Delay(10); require(callbacks.load() == 0, "device remains paused until mixer ready");
    ok(description->start(&output), "start callback failure fixture");
    for (int i = 0; i < 100 && callbacks.load() == 0; ++i) SDL_Delay(2);
    require(callbacks.load() > 0, "real SDL callback executed");
    require(description->update(&output) == FMOD_ERR_INTERNAL, "mixer error propagated, not fake success");
    ok(description->stop(&output), "stop drains callbacks");
    unsigned int stopped = callbacks.load(); SDL_Delay(20);
    require(callbacks.load() == stopped, "no callbacks after stop");
    ok(description->close(&output), "close callback failure fixture");
    require(output.plugindata == nullptr, "close clears owned data");
    ok(description->close(&output), "idempotent cleanup");
}
static std::atomic<unsigned int> faultCallbacks{0};
static unsigned int faultOpens = 0, faultStarts = 0, faultStops = 0, faultCloses = 0;
static FMOD_RESULT F_CALLBACK faultMixer(FMOD_OUTPUT_STATE *output, void *buffer, unsigned int frames)
{
    faultCallbacks.fetch_add(1);
    return FMOD::Output::mixCallback(output, buffer, frames);
}
static FMOD_RESULT F_CALLBACK openThenFail(FMOD_OUTPUT_STATE *output, int driver, FMOD_INITFLAGS flags,
    int *rate, int channels, FMOD_SOUND_FORMAT *format, int length, int buffers, void *extra)
{
    output->readfrommixer = faultMixer;
    FMOD_RESULT result = FMOD::getSDLDescription()->init(output, driver, flags, rate, channels, format, length, buffers, extra);
    if (result != FMOD_OK) return result;
    ++faultOpens;
    require(output->plugindata != nullptr, "fault injected only after actual SDL open");
    return FMOD_ERR_INTERNAL;
}
static FMOD_RESULT F_CALLBACK startThenFail(FMOD_OUTPUT_STATE *output)
{
    ok(FMOD::getSDLDescription()->start(output), "fault fixture starts actual SDL device");
    ++faultStarts;
    // Require a callback while init is still uncommitted, not just device-open success.
    for (int i = 0; i < 100 && faultCallbacks.load() == 0; ++i) SDL_Delay(2);
    require(faultCallbacks.load() > 0, "fault after start observes real shared mixer callbacks");
    return FMOD_ERR_INTERNAL;
}
static FMOD_RESULT F_CALLBACK openForStartFault(FMOD_OUTPUT_STATE *output, int driver, FMOD_INITFLAGS flags,
    int *rate, int channels, FMOD_SOUND_FORMAT *format, int length, int buffers, void *extra)
{
    output->readfrommixer = faultMixer;
    FMOD_RESULT result = FMOD::getSDLDescription()->init(output, driver, flags, rate, channels, format, length, buffers, extra);
    if (result == FMOD_OK) ++faultOpens;
    return result;
}
static FMOD_RESULT F_CALLBACK faultStop(FMOD_OUTPUT_STATE *output)
{ ++faultStops; return FMOD::getSDLDescription()->stop(output); }
static FMOD_RESULT F_CALLBACK faultClose(FMOD_OUTPUT_STATE *output)
{ ++faultCloses; return FMOD::getSDLDescription()->close(output); }
static void failedInitChecks()
{
    for (bool afterStart : {false, true})
    {
        faultCallbacks.store(0);
        faultOpens = faultStarts = faultStops = faultCloses = 0;
        FMOD::System *system = nullptr;
        ok(FMOD::System_Create(&system), "create failed-init fixture");
        ok(system->setOutput(FMOD_OUTPUTTYPE_AUTODETECT), "select actual SDL backend for fault");
        FMOD::SystemI *internal = nullptr;
        ok(FMOD::SystemI::validate(system, &internal), "validate real internal system");
        // Wrap this system's real EX callbacks, not the global description or a fake output.
        internal->mOutput->mDescription.init = afterStart ? openForStartFault : openThenFail;
        internal->mOutput->mDescription.start = startThenFail;
        internal->mOutput->mDescription.stop = faultStop;
        internal->mOutput->mDescription.close = faultClose;
        require(system->init(16, FMOD_INIT_NORMAL, nullptr) == FMOD_ERR_INTERNAL, "injected init failure returned");
        require(!internal->mInitialized && internal->mOutput->plugindata, "failure leaves real partial backend for release");
        require(faultOpens == 1 && faultStarts == (afterStart ? 1u : 0u), "fault reached requested lifecycle stage");
        ok(system->release(), "release partially initialized system");
        unsigned int stopped = faultCallbacks.load();
        SDL_Delay(30);
        require(faultCallbacks.load() == stopped, "no dangling callbacks after failed-init release");
        require(faultStops == 1 && faultCloses == 1, "partial backend stopped and closed exactly once");
        require(SDL_WasInit(SDL_INIT_AUDIO) == 0, "partial-init audio subsystem reference released");
        // A fresh real playback device must be available after each failed init.
        ok(FMOD::System_Create(&system), "create recovery system");
        ok(system->init(8, FMOD_INIT_NORMAL, nullptr), "open playback after failed-init release");
        ok(system->release(), "release recovery system");
        std::printf("PASS failed init after %s: release stops/closes backend; callbacks drained; recovery opens\n", afterStart ? "start" : "open");
    }
}
int main(int argc, char **argv)
{
    require(std::getenv("SDL_AUDIODRIVER") != nullptr, "explicit non-physical SDL driver required");
    const bool failure = argc > 1 && !std::strcmp(argv[1], "--unavailable");
    require(!std::strcmp(std::getenv("SDL_AUDIODRIVER"), failure ? "wowdecomp-invalid-driver" : "dummy"), "only test audio drivers allowed");
    platformChecks();
    if (!failure) { outputFailureChecks(); failedInitChecks(); }
    FMOD::System *system = nullptr;
    FMOD_RESULT created = FMOD::System_Create(&system);
    if (failure)
    {
        if (created == FMOD_OK)
        {
            require(system->init(16, FMOD_INIT_NORMAL, nullptr) != FMOD_OK, "device failure must not succeed");
            ok(system->release(), "release after init failure");
        }
        else require(created == FMOD_ERR_OUTPUT_INIT || created == FMOD_ERR_OUTPUT_DRIVERCALL, "explicit output create failure");
        std::puts("PASS unavailable driver returns genuine failure"); return 0;
    }
    ok(created, "create real FMOD system");
    // Match SoundEngine's dedicated enumeration system without consuming a device.
    FMOD::System *enumerator = nullptr;
    ok(FMOD::System_Create(&enumerator), "create enumeration system");
    int drivers = 0;
    ok(enumerator->getNumDrivers(&drivers), "enumerate without opening playback");
    require(drivers > 0, "SDL default endpoint enumerated");
    ok(system->init(16, FMOD_INIT_NORMAL, nullptr), "initialize SDL dummy real mixer");
    FMOD_OUTPUTTYPE type;
    ok(system->getOutput(&type), "query output"); require(type == FMOD_OUTPUTTYPE_UNKNOWN, "SDL output, never NOSOUND");
    int recording = -1;
    require(system->getRecordNumDrivers(&recording) == FMOD_ERR_UNSUPPORTED, "capture unsupported");
    FMOD_DSP_DESCRIPTION description = {};
    std::strcpy(description.name, "NativeSoundTest observe"); description.channels = 0; description.read = observe;
    FMOD::DSP *dsp = nullptr;
    ok(system->createDSP(&description, &dsp), "create observer DSP");
    ok(system->addDSP(dsp), "attach observer to real mixer");
    makeFixture();
    FMOD_CREATESOUNDEXINFO info = {};
    info.cbsize = sizeof(info); info.useropen = openFixture; info.userclose = closeFixture;
    info.userread = readFixture; info.userseek = seekFixture;
    FMOD::Sound *sound = nullptr;
    // Callback-only name is intentionally not a POSIX path. This exercises FMOD's
    // shared file-callback seam used by SoundEngine's SFile ownership transfer.
    ok(system->createSound("fixture-not-a-real-path", FMOD_SOFTWARE | FMOD_2D | FMOD_LOOP_NORMAL, &info, &sound), "decode generated WAV via callbacks");
    FMOD::Channel *channel = nullptr;
    ok(system->playSound(FMOD_CHANNEL_FREE, sound, false, &channel), "play decoded PCM");
    for (int i = 0; i < 100 && mixed.load() == 0; ++i)
    { ok(system->update(), "mixer update"); SDL_Delay(10); }
    require(mixed.load() > 0, "actual real mixer produces nonzero generated audio");
    require(opens && reads && seeks, "file callbacks exercised");
    ok(channel->stop(), "stop channel"); ok(sound->release(), "release sound");
    ok(dsp->remove(), "remove observer"); ok(dsp->release(), "release observer");
    const unsigned int stoppedSamples = mixed.load();
    ok(system->close(), "stop and close output");
    SDL_Delay(20);
    require(mixed.load() == stoppedSamples, "real mixer does not run after close");
    ok(system->init(8, FMOD_INIT_NORMAL, nullptr), "reopen output");
    ok(system->close(), "close reopened output"); ok(system->release(), "release system");
    ok(enumerator->release(), "release enumeration system");
    require(opens == closes, "balanced callback ownership");
    std::printf("PASS SDL dummy real mixer: %u nonzero samples; file callbacks %u/%u/%u/%u; reopen/close\n", mixed.load(), opens, reads, seeks, closes);
}
