// Native-extension algorithm; the original Windows translation unit is unchanged.
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <cerrno>

#include "Engine/Source/BlizzardCore/Include/Blizzard/Util/MD5.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Streaming/Manifests.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Streaming/Storage.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Streaming/Streaming.h"
#include "Engine/Source/BlizzardCore/Source/System/Debug/ErrorStack.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/File/Filestack.h"
#include "Engine/Source/BlizzardCore/Source/System/File/Filestack_Streaming.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Log.h"
#include "Engine/Source/BlizzardCore/Source/System/Memory/MemoryStorm.h"
#include "Engine/Source/BlizzardCore/Source/System/String/Path.h"
#include "Engine/Source/BlizzardCore/Source/System/String/String.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"

// STATIC: 0x009E6204
static const int32_t s_queueOrder[6] = { 0, 1, 0, 2, 0, 1 };

// STATIC: 0x009E621C
static const int32_t s_queueGroupBase[2] = { 0, 4 };

// STATIC: 0x00AB9344
static bool s_useBackgroundQueues = true;

// STATIC: 0x00B32530

// Constructed by real initialization; native operations lock before inspecting queues.
static Blizzard::Lock::Mutex s_queueLock;

// STATIC: 0x00B32554
static Blizzard::Streaming::FILE_THREAD_CMD* s_currentCommands[2];

// STATIC: 0x00B3255C
static std::deque<Blizzard::Streaming::FILE_THREAD_CMD>* s_queues[7];

// STATIC: 0x00B325DC

static Blizzard::Lock::Event s_queueEvent;

// STATIC: 0x00B38294
static Blizzard::Streaming::DigestStateMap* s_digestStates;

// STATIC: 0x00B3829C
static int32_t s_commandsTaken;

// STATIC: 0x00B382A0
static int32_t s_commandsFreed;

// STATIC: 0x00B38A54
static int32_t s_queueRotation[2];

// OFFSET: 0x00437ED0
bool Blizzard::Streaming::CancelQueue(int32_t priority) {

    if (!s_queueEvent) { errno = ENOTSUP; return false; }
    Blizzard::Lock::ScopedMutex lock(s_queueLock);
    if (!(priority >= 0 && priority < 7)) {
        return false;
    }

    std::deque<FILE_THREAD_CMD>& queue = *s_queues[priority];

    for (std::deque<FILE_THREAD_CMD>::iterator it = queue.begin(); it != queue.end(); ++it) {
        if (it->m_completion && it->m_type == 2) {
            it->m_completion->m_result = static_cast<int32_t>(s_queues[priority]->size());
            it->m_completion->m_done = true;

            Blizzard::Lock::EventSet(*it->m_completion->m_event);

            it->m_completion = nullptr;
        }

        it->m_type = 0;
    }

    for (DigestStateMap::iterator it = s_digestStates->begin(); it != s_digestStates->end(); ++it) {
        if (it->second.m_state && it->second.m_priority == priority) {
            it->second.m_state = 0;
        }
    }

    return true;
}

// OFFSET: 0x00438120
bool Blizzard::Streaming::PopCommand(FILE_THREAD_CMD* command, int32_t queue) {

    Blizzard::Lock::ScopedMutex lock(s_queueLock);
    if (!(command && queue >= 0 && queue < 7)) {
        return false;
    }

    if (!s_queues[queue]->size()) {
        return false;
    }

    *command = s_queues[queue]->front();
    s_queues[queue]->pop_front();
    s_commandsTaken++;

    return true;
}

// OFFSET: 0x004381E0
bool Blizzard::Streaming::SelectQueue(FILE_THREAD_CMD* command, int32_t group, int32_t* queue) {

    Blizzard::Lock::ScopedMutex lock(s_queueLock);
    if (!(command && queue && group >= 0 && group < 2)) {
        return false;
    }

    if (group >= 1 && !s_useBackgroundQueues) {
        return false;
    }

    int32_t base = s_queueGroupBase[group];

    for (int32_t i = 0; i < 6; i++) {
        *queue = base + s_queueOrder[s_queueRotation[group]];
        s_queueRotation[group] = (s_queueRotation[group] + 1) % 6;

        if (PopCommand(command, *queue)) {
            return true;
        }
    }

    return false;
}

// OFFSET: 0x00438260
int32_t Blizzard::Streaming::GetNextCommand(FILE_THREAD_CMD** command, int32_t thread) {

    Blizzard::Lock::ScopedMutex lock(s_queueLock);
    if (!(command && thread >= 0 && thread < 2)) {
        return 7;
    }

    FILE_THREAD_CMD* next = new FILE_THREAD_CMD();
    int32_t queue = 7;

    if (SelectQueue(next, 0, &queue) || (queue = 3, PopCommand(next, 3)) || SelectQueue(next, 1, &queue)) {
        s_currentCommands[thread] = next;
        *command = next;
        return queue;
    }

    delete next;
    s_commandsFreed++;
    return 7;
}

// OFFSET: 0x0043AF30
bool Blizzard::Streaming::RemoveCompletion(Completion* completion, int32_t priority) {

    if (!s_queueEvent) { errno = ENOTSUP; return false; }
    Blizzard::Lock::ScopedMutex lock(s_queueLock);
    if (!(completion && priority >= 0 && priority < 7)) {
        return false;
    }

    if (!s_queues[priority]->size()) {
        return false;
    }

    std::deque<FILE_THREAD_CMD>& queue = *s_queues[priority];

    for (std::deque<FILE_THREAD_CMD>::iterator it = queue.end(); it != queue.begin();) {
        --it;

        if (it->m_completion == completion) {
            it->m_completion = nullptr;

            return true;
        }
    }

    return false;
}

// OFFSET: 0x00444AA0
bool Blizzard::Streaming::QueueCompletion(Completion* completion, int32_t priority) {

    if (!s_queueEvent) { errno = ENOTSUP; return false; }
    Blizzard::Lock::ScopedMutex lock(s_queueLock);
    if (!(completion && priority >= 0 && priority < 7)) {
        return false;
    }

    if (!s_queues[priority]->size()) {
        completion->m_result = 0;
        completion->m_done = false;
        completion->m_success = false;
        return false;
    }

    FILE_THREAD_CMD command;
    command.m_fs = nullptr;
    command.m_type = 2;
    command.m_file = nullptr;
    command.m_range = nullptr;
    command.m_completion = completion;

    s_queues[priority]->push_back(command);

    Blizzard::Lock::EventSet(s_queueEvent);

    return true;
}
