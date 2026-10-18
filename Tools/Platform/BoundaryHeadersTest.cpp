#include <type_traits>
#include "Engine/Source/Platform/Platform.h"
#include "Engine/Source/Storm/H/SCritSect.h"
#include "Engine/Source/Storm/Source/SLock.h"
#include "Engine/Source/Storm/Source/SThread.h"
#include "Engine/Source/BlizzardCore/Source/System/File/File.h"
#include "Engine/Source/BlizzardCore/Source/System/Hardware/Hardware.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/Thread.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Time.h"
#include "Engine/Source/BlizzardCore/Source/System/Debug/Native.h"
#include "Engine/Source/BlizzardCore/Source/System/Log/Native.h"
#include "Engine/Source/BlizzardCore/Source/System/Time/Native.h"
#include "Engine/Source/Os/Window.h"
#include "Engine/Source/Os/ProcessorFeatures.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Event/Types.h"
#include "WoW/Source/Native/Startup.h"

// Compile and link declarations without providing fake OS implementations.
// These contracts must not require Linux/SDL/DXVK/curl providers to be present.
static_assert(std::is_same_v<decltype(&System_Debug::Native::DebuggerPresent), bool (*)()>);
static_assert(std::is_same_v<decltype(&System_Debug::Native::OpenAppend), FILE* (*)(const char*)>);
static_assert(std::is_same_v<decltype(&System_Time::NativeTimestamp), Blizzard::Time::Timestamp (*)()>);
static_assert(std::is_same_v<decltype(&NativeStartupInitializeFloatingPoint), bool (*)()>);
static_assert(std::is_same_v<decltype(&OsGuiGetWindow), void* (*)(int32_t)>);
static_assert(std::is_same_v<decltype(&Blizzard::Thread::WaitForever), bool (*)(Blizzard::Thread::ThreadRecord*)>);
#if WOW_PLATFORM_WINDOWS
static_assert(std::is_same_v<decltype(Blizzard::Thread::ThreadRecord::m_handle), HANDLE>);
static_assert(std::is_same_v<decltype(Blizzard::Thread::ThreadRecord::m_state), volatile int32_t>);
#else
#include "Engine/Source/BlizzardCore/Source/System/Thread/Posix/Platform.h"
static_assert(std::is_same_v<decltype(&System_Thread::Platform::CurrentId), uint32_t (*)()>);
static_assert(std::is_same_v<decltype(Blizzard::Thread::ThreadRecord::m_handle), void*>);
static_assert(std::is_same_v<decltype(Blizzard::Thread::ThreadRecord::m_state), std::atomic<int32_t>>);
static_assert(std::is_same_v<decltype(&System_Hardware::GetProcessorCount), uint32_t (*)()>);
#endif
int main() { return 0; }
