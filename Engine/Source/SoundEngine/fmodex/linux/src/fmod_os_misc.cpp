#include "fmod_os_misc.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <new>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct FMOD_OS_CRITICALSECTION { pthread_mutex_t mutex; };
struct FMOD_OS_SEMAPHORE { sem_t semaphore; };
int gSizeofCriticalSection = sizeof(FMOD_OS_CRITICALSECTION);
int gSizeofSemaphore = sizeof(FMOD_OS_SEMAPHORE);

void *FMOD_OS_Memory_Alloc(int size, FMOD_MEMORY_TYPE) { return size > 0 ? std::malloc(size) : nullptr; }
void *FMOD_OS_Memory_Realloc(void *p, int size, FMOD_MEMORY_TYPE) { return size >= 0 ? std::realloc(p, size) : nullptr; }
void FMOD_OS_Memory_Free(void *p, FMOD_MEMORY_TYPE) { std::free(p); }
// The original platform does not implement GetMemoryUsed; middleware accounts allocations itself.

FMOD_RESULT FMOD_OS_File_Open(const char *name, char *mode, int unicode, unsigned int *size, void **handle)
{
    if (!name || !mode || !size || !handle) return FMOD_ERR_INVALID_PARAM;
    *handle = nullptr;
    if (unicode || std::strcmp(mode, "rb")) return FMOD_ERR_UNSUPPORTED;
    FILE *file = std::fopen(name, "rb");
    if (!file) return errno == ENOENT ? FMOD_ERR_FILE_NOTFOUND : FMOD_ERR_FILE_BAD;
    struct stat info;
    if (fstat(fileno(file), &info) || info.st_size < 0 || info.st_size > UINT_MAX)
    { std::fclose(file); return FMOD_ERR_FILE_BAD; }
    *size = static_cast<unsigned int>(info.st_size);
    *handle = file;
    return FMOD_OK;
}
FMOD_RESULT FMOD_OS_File_Close(void *h) { return h && !std::fclose(static_cast<FILE *>(h)) ? FMOD_OK : FMOD_ERR_FILE_BAD; }
FMOD_RESULT FMOD_OS_File_Read(void *h, void *buf, unsigned int count, unsigned int *read)
{
    if (!h || !buf || !read) return FMOD_ERR_INVALID_PARAM;
    *read = std::fread(buf, 1, count, static_cast<FILE *>(h));
    if (std::ferror(static_cast<FILE *>(h))) return FMOD_ERR_FILE_BAD;
    return *read == count ? FMOD_OK : FMOD_ERR_FILE_EOF;
}
FMOD_RESULT FMOD_OS_File_Seek(void *h, unsigned int offset)
{ return h && !fseeko(static_cast<FILE *>(h), offset, SEEK_SET) ? FMOD_OK : FMOD_ERR_FILE_COULDNOTSEEK; }
FMOD_RESULT FMOD_OS_File_Cancel(void *) { return FMOD_ERR_UNSUPPORTED; }
FMOD_RESULT FMOD_OS_File_DriveStatus() { return FMOD_OK; }
FMOD_RESULT FMOD_OS_Debug_OutputStr(const char *s) { if (!s) return FMOD_ERR_INVALID_PARAM; std::fputs(s, stderr); return FMOD_OK; }
static FMOD_RESULT clockValue(unsigned int *value, unsigned long long divisor)
{
    if (!value) return FMOD_ERR_INVALID_PARAM;
    timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return FMOD_ERR_INTERNAL;
    *value = static_cast<unsigned int>((static_cast<unsigned long long>(t.tv_sec) * 1000000000ULL + t.tv_nsec) / divisor);
    return FMOD_OK;
}
FMOD_RESULT FMOD_OS_Time_GetNs(unsigned int *value) { return clockValue(value, 1); }
FMOD_RESULT FMOD_OS_Time_GetMs(unsigned int *value) { return clockValue(value, 1000000); }
FMOD_RESULT FMOD_OS_Time_Sleep(unsigned int ms)
{
    timespec t = {static_cast<time_t>(ms / 1000), static_cast<long>(ms % 1000) * 1000000};
    while (nanosleep(&t, &t)) if (errno != EINTR) return FMOD_ERR_INTERNAL;
    return FMOD_OK;
}
struct NativeThread { pthread_t thread; THREAD_RETURNTYPE (*callback)(void *); void *param; };
static void *threadEntry(void *p) { auto *t = static_cast<NativeThread *>(p); t->callback(t->param); return nullptr; }
FMOD_RESULT FMOD_OS_Thread_GetCurrentID(FMOD_UINT_NATIVE *id)
{ if (!id) return FMOD_ERR_INVALID_PARAM; *id = static_cast<FMOD_UINT_NATIVE>(syscall(SYS_gettid)); return FMOD_OK; }
FMOD_RESULT FMOD_OS_Thread_Create(const char *, THREAD_RETURNTYPE (*callback)(void *), void *param,
                                FMOD_THREAD_PRIORITY, void *stack, int, void **handle)
{
    if (!callback || !handle) return FMOD_ERR_INVALID_PARAM;
    *handle = nullptr;
    if (stack) return FMOD_ERR_UNSUPPORTED;
    auto *t = new (std::nothrow) NativeThread;
    if (!t) return FMOD_ERR_MEMORY;
    t->callback = callback; t->param = param;
    // Use the system stack size: legacy 8-32 KiB stacks are unsafe with modern codecs.
    // Unprivileged threads retain normal scheduling; never request realtime privileges.
    if (pthread_create(&t->thread, nullptr, threadEntry, t)) { delete t; return FMOD_ERR_INTERNAL; }
    *handle = t;
    return FMOD_OK;
}
FMOD_RESULT FMOD_OS_Thread_Destroy(void *h)
{
    if (!h) return FMOD_ERR_INVALID_PARAM;
    auto *t = static_cast<NativeThread *>(h);
    if (pthread_join(t->thread, nullptr)) return FMOD_ERR_INTERNAL;
    delete t; return FMOD_OK;
}
FMOD_RESULT FMOD_OS_CriticalSection_Create(FMOD_OS_CRITICALSECTION **out, bool)
{
    if (!out) return FMOD_ERR_INVALID_PARAM;
    *out = nullptr;
    auto *c = new (std::nothrow) FMOD_OS_CRITICALSECTION;
    if (!c) return FMOD_ERR_MEMORY;
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr)) { delete c; return FMOD_ERR_INTERNAL; }
    int result = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (!result) result = pthread_mutex_init(&c->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (result) { delete c; return FMOD_ERR_INTERNAL; }
    *out = c; return FMOD_OK;
}
FMOD_RESULT FMOD_OS_CriticalSection_Free(FMOD_OS_CRITICALSECTION *c)
{ if (!c || pthread_mutex_destroy(&c->mutex)) return FMOD_ERR_INTERNAL; delete c; return FMOD_OK; }
FMOD_RESULT FMOD_OS_CriticalSection_Enter(FMOD_OS_CRITICALSECTION *c)
{ return c && !pthread_mutex_lock(&c->mutex) ? FMOD_OK : FMOD_ERR_INTERNAL; }
FMOD_RESULT FMOD_OS_CriticalSection_Leave(FMOD_OS_CRITICALSECTION *c)
{ return c && !pthread_mutex_unlock(&c->mutex) ? FMOD_OK : FMOD_ERR_INTERNAL; }
FMOD_RESULT FMOD_OS_Semaphore_Create(FMOD_OS_SEMAPHORE **out)
{
    if (!out) return FMOD_ERR_INVALID_PARAM;
    *out = new (std::nothrow) FMOD_OS_SEMAPHORE;
    if (!*out) return FMOD_ERR_MEMORY;
    if (sem_init(&(*out)->semaphore, 0, 0)) { delete *out; *out = nullptr; return FMOD_ERR_INTERNAL; }
    return FMOD_OK;
}
FMOD_RESULT FMOD_OS_Semaphore_Free(FMOD_OS_SEMAPHORE *s)
{ if (!s || sem_destroy(&s->semaphore)) return FMOD_ERR_INTERNAL; delete s; return FMOD_OK; }
FMOD_RESULT FMOD_OS_Semaphore_Wait(FMOD_OS_SEMAPHORE *s)
{ if (!s) return FMOD_ERR_INVALID_PARAM; while (sem_wait(&s->semaphore)) if (errno != EINTR) return FMOD_ERR_INTERNAL; return FMOD_OK; }
FMOD_RESULT FMOD_OS_Semaphore_Signal(FMOD_OS_SEMAPHORE *s, bool)
{ return s && !sem_post(&s->semaphore) ? FMOD_OK : FMOD_ERR_INTERNAL; }
FMOD_RESULT FMOD_OS_Library_Load(const char *name, FMOD_OS_LIBRARY **out)
{
    if (!name || !out) return FMOD_ERR_INVALID_PARAM;
    *out = reinterpret_cast<FMOD_OS_LIBRARY *>(dlopen(name, RTLD_NOW | RTLD_LOCAL));
    return *out ? FMOD_OK : FMOD_ERR_PLUGIN_MISSING;
}
FMOD_RESULT FMOD_OS_Library_GetProcAddress(FMOD_OS_LIBRARY *h, const char *name, void **out)
{ if (!h || !name || !out) return FMOD_ERR_INVALID_PARAM; *out = dlsym(h, name); return *out ? FMOD_OK : FMOD_ERR_PLUGIN; }
FMOD_RESULT FMOD_OS_Library_Free(FMOD_OS_LIBRARY *h) { return h && !dlclose(h) ? FMOD_OK : FMOD_ERR_PLUGIN; }
FMOD_RESULT FMOD_OS_CheckDriverList(bool *changed)
{ if (!changed) return FMOD_ERR_INVALID_PARAM; *changed = false; return FMOD_ERR_UNSUPPORTED; }
