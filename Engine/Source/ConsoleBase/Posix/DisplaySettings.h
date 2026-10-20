#ifndef ENGINE_SOURCE_CONSOLEBASE_POSIX_DISPLAYSETTINGS_H
#define ENGINE_SOURCE_CONSOLEBASE_POSIX_DISPLAYSETTINGS_H
#include <strings.h>

inline bool ConsoleIsRetiredDisplayCVar(const char* name) {
    return name && (!strcasecmp(name, "gxResolution") || !strcasecmp(name, "gxRefresh"));
}
#endif
