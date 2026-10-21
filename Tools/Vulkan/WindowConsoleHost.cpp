#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include <stdexcept>

// Console UI/history are host side effects. ParseCommand and the registered SET
// handler remain production code, as do CVar::Load and ConsoleDeviceInitialize.
void ConsoleWrite(const char*, COLOR_T) {}
void ConsoleWriteA(const char*, COLOR_T, ...) {}
void ConsolePrintf(const char*, ...) {}
void ConsolePostClose() {}
void ConsoleCommandExecute(const char* line, int32_t) {
    const char* command;
    char arguments[1024];
    auto* entry = ParseCommand(line, &command, arguments, sizeof(arguments));
    if (!entry || !entry->m_handler(command, arguments))
        throw std::runtime_error("Production config command failed");
}
