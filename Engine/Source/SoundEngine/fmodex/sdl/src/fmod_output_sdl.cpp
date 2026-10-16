#include "fmod_output_sdl.h"
#include <SDL.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

namespace
{
struct SDLPlayback
{
    SDL_AudioDeviceID device = 0;
    FMOD_OUTPUT_STATE *output = nullptr;
    FMOD_OUTPUT_READFROMMIXER mixer = nullptr;
    unsigned int channels = 0;
    std::atomic<FMOD_RESULT> error{FMOD_OK};
};
FMOD_RESULT audioError(const char *operation)
{
    std::fprintf(stderr, "FMOD SDL2 %s: %s\n", operation, SDL_GetError());
    return FMOD_ERR_OUTPUT_INIT;
}
FMOD_RESULT F_CALLBACK numDrivers(FMOD_OUTPUT_STATE *, int *count)
{
    if (!count) return FMOD_ERR_INVALID_PARAM;
    *count = 0;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) return audioError("audio initialization failed");
    int n = SDL_GetNumAudioDevices(0);
    // Index zero is SDL's default playback endpoint, including virtual drivers.
    if (n >= 0) *count = n + 1;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return n < 0 ? FMOD_ERR_OUTPUT_DRIVERCALL : FMOD_OK;
}
FMOD_RESULT F_CALLBACK driverName(FMOD_OUTPUT_STATE *, int id, char *name, int len)
{
    if (!name || len <= 0 || id < 0) return FMOD_ERR_INVALID_PARAM;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) return audioError("enumeration failed");
    const char *value = id == 0 ? "SDL2 default playback device" : SDL_GetAudioDeviceName(id - 1, 0);
    if (value) std::snprintf(name, len, "%s", value);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return value ? FMOD_OK : FMOD_ERR_INVALID_PARAM;
}
FMOD_RESULT F_CALLBACK driverCaps(FMOD_OUTPUT_STATE *, int id, FMOD_CAPS *caps)
{
    int count;
    if (!caps) return FMOD_ERR_INVALID_PARAM;
    CHECK_RESULT(numDrivers(nullptr, &count));
    if (id < 0 || id >= count) return FMOD_ERR_INVALID_PARAM;
    *caps = FMOD_CAPS_NONE;
    return FMOD_OK;
}
void audioCallback(void *userdata, Uint8 *buffer, int bytes)
{
    auto *data = static_cast<SDLPlayback *>(userdata);
    unsigned int frameBytes = data->channels * sizeof(float);
    if (bytes < 0 || static_cast<unsigned int>(bytes) % frameBytes)
    {
        data->error.store(FMOD_ERR_OUTPUT_FORMAT);
        if (bytes > 0) std::memset(buffer, 0, bytes);
        return;
    }
    FMOD_RESULT result = data->mixer(data->output, buffer, bytes / frameBytes);
    if (result != FMOD_OK)
    {
        // Never deliver uninitialized audio on mixer failure; update reports the error.
        data->error.store(result);
        std::memset(buffer, 0, bytes);
    }
}
FMOD_RESULT F_CALLBACK init(FMOD_OUTPUT_STATE *output, int driver, FMOD_INITFLAGS,
                           int *rate, int channels, FMOD_SOUND_FORMAT *format, int length, int, void *)
{
    if (!output || output->plugindata || !output->readfrommixer || !rate || !format ||
        *rate <= 0 || channels < 1 || channels > 8 || length <= 0 || length > 32768 || driver < 0)
        return FMOD_ERR_INVALID_PARAM;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) return audioError("audio initialization failed");
    auto *data = new (std::nothrow) SDLPlayback;
    if (!data) { SDL_QuitSubSystem(SDL_INIT_AUDIO); return FMOD_ERR_MEMORY; }
    data->output = output;
    // FMOD resets the public callback field during update; keep the immutable
    // callback chosen at init instead of racing those redundant pointer writes.
    data->mixer = output->readfrommixer;
    data->channels = channels;
    SDL_AudioSpec desired = {}, obtained = {};
    desired.freq = *rate; desired.format = AUDIO_F32SYS; desired.channels = channels;
    // SDL callback buffers have power-of-two sample counts.
    unsigned int samples = 1;
    while (samples < static_cast<unsigned int>(length)) samples <<= 1;
    desired.samples = samples; desired.callback = audioCallback; desired.userdata = data;
    const char *name = driver == 0 ? nullptr : SDL_GetAudioDeviceName(driver - 1, 0);
    if (driver > 0 && !name)
    { delete data; SDL_QuitSubSystem(SDL_INIT_AUDIO); return FMOD_ERR_INVALID_PARAM; }
    data->device = SDL_OpenAudioDevice(name, 0, &desired, &obtained, 0);
    if (!data->device)
    {
        FMOD_RESULT result = audioError("device unavailable");
        delete data; SDL_QuitSubSystem(SDL_INIT_AUDIO); return result;
    }
    // OpenAudioDevice returns paused: start is called only after FMOD's mixer exists.
    *format = FMOD_SOUND_FORMAT_PCMFLOAT;
    output->plugindata = data;
    return FMOD_OK;
}
FMOD_RESULT F_CALLBACK start(FMOD_OUTPUT_STATE *output)
{
    auto *data = static_cast<SDLPlayback *>(output->plugindata);
    if (!data) return FMOD_ERR_UNINITIALIZED;
    SDL_PauseAudioDevice(data->device, 0); return FMOD_OK;
}
FMOD_RESULT F_CALLBACK stop(FMOD_OUTPUT_STATE *output)
{
    auto *data = static_cast<SDLPlayback *>(output->plugindata);
    if (!data) return FMOD_ERR_UNINITIALIZED;
    SDL_PauseAudioDevice(data->device, 1);
    // Pausing waits for the in-flight callback before mixer teardown.
    return FMOD_OK;
}
FMOD_RESULT F_CALLBACK close(FMOD_OUTPUT_STATE *output)
{
    auto *data = static_cast<SDLPlayback *>(output->plugindata);
    if (!data) return FMOD_OK;
    SDL_CloseAudioDevice(data->device);
    delete data; output->plugindata = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO); return FMOD_OK;
}
FMOD_RESULT F_CALLBACK update(FMOD_OUTPUT_STATE *output)
{
    auto *data = static_cast<SDLPlayback *>(output->plugindata);
    if (!data) return FMOD_ERR_UNINITIALIZED;
    if (SDL_GetAudioDeviceStatus(data->device) == SDL_AUDIO_STOPPED) return FMOD_ERR_OUTPUT_DRIVERCALL;
    return data->error.load();
}
FMOD_RESULT F_CALLBACK getHandle(FMOD_OUTPUT_STATE *output, void **handle)
{
    if (!handle) return FMOD_ERR_INVALID_PARAM;
    auto *data = static_cast<SDLPlayback *>(output->plugindata);
    if (!data) return FMOD_ERR_UNINITIALIZED;
    *handle = &data->device; return FMOD_OK;
}
}
namespace FMOD
{
FMOD_OUTPUT_DESCRIPTION_EX *getSDLDescription()
{
    static FMOD_OUTPUT_DESCRIPTION_EX description = [] {
        FMOD_OUTPUT_DESCRIPTION_EX d = {};
        d.name = "SDL2 native playback"; d.version = 0x00010000;
        d.getnumdrivers = numDrivers; d.getdrivername = driverName; d.getdrivercaps = driverCaps;
        d.init = init; d.close = close; d.start = start; d.stop = stop;
        d.update = update; d.gethandle = getHandle;
        d.mType = FMOD_OUTPUTTYPE_UNKNOWN; d.mSize = sizeof(Output);
        return d;
    }();
    return &description;
}
}
