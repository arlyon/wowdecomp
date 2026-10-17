#include <cerrno>
#include <limits>
#include <pthread.h>
#include "Engine/Source/Os/Tls.h"

namespace {
constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();
}

uint32_t OsTlsAlloc() {
    pthread_key_t key;
    const int error = pthread_key_create(&key, nullptr);
    if (error) {
        errno = error;
        return InvalidIndex;
    }
    // Darwin's key type can be wider than this legacy API. Check the value,
    // not sizeof(pthread_key_t), and never leak a key we cannot represent.
    if (static_cast<uint64_t>(key) >= InvalidIndex) {
        pthread_key_delete(key);
        errno = EOVERFLOW;
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
