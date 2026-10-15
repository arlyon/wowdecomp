#include <cerrno>
#include <limits>
#include <pthread.h>
#include "Engine/Source/Os/Tls.h"

namespace {
constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();
static_assert(sizeof(pthread_key_t) <= sizeof(uint32_t), "TLS keys must fit the existing OS API");
}

uint32_t OsTlsAlloc() {
    pthread_key_t key;
    const int error = pthread_key_create(&key, nullptr);
    if (error) {
        errno = error;
        return InvalidIndex;
    }
    return static_cast<uint32_t>(key);
}

void OsTlsFree(uint32_t index) {
    if (index == InvalidIndex) {
        errno = EINVAL;
        return;
    }
    const int error = pthread_key_delete(static_cast<pthread_key_t>(index));
    if (error) {
        errno = error;
    }
}

void* OsTlsGetValue(uint32_t index) {
    if (index == InvalidIndex) {
        errno = EINVAL;
        return nullptr;
    }
    return pthread_getspecific(static_cast<pthread_key_t>(index));
}

int32_t OsTlsSetValue(uint32_t index, void* value) {
    if (index == InvalidIndex) {
        errno = EINVAL;
        return 0;
    }
    const int error = pthread_setspecific(static_cast<pthread_key_t>(index), value);
    if (error) {
        errno = error;
    }
    return error == 0;
}
