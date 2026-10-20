#ifndef ENGINE_SOURCE_OS_WINDOW_H
#define ENGINE_SOURCE_OS_WINDOW_H

#include <cstdint>

// Window services do not require the native tool-dialog classes in Gui.h.
void OsGuiBeep();
void* OsGuiGetWindow(int32_t type);
int32_t OsGuiIsModifierKeyDown(int32_t key);
int32_t OsGuiProcessMessage(void* message);
void OsGuiSetGxWindow(void* window);
int32_t OsGuiMessageBox(void* parentWindow, int32_t style, const char* message, const char* title);
void OsGuiSetWindowTitle(void* window, const char* title);

#if !defined(_WIN32)
#include "Engine/Source/Os/Posix/WindowPlacement.h"
#include "Engine/Source/Tempest/C2iVector.h"
bool OsGuiGetWindowMode(void* window, OsWindowMode& mode);
bool OsGuiGetWindowDesktopMode(void* window, const OsWindowPlacement& placement, C2iVector& size, uint32_t& refresh);
bool OsGuiSupportsExclusiveFullscreen();
bool OsGuiSetWindowMode(void* window, bool windowed, int maximize, const C2iVector& size, uint32_t refresh);
bool OsGuiSetSavedWindowPlacement(const OsWindowPlacement& placement);
OsWindowPlacement OsGuiGetSavedWindowPlacement();
bool OsGuiGetWindowPlacement(void* window, OsWindowPlacement& placement);
void OsGuiRestoreWindowPlacement(void* window, const OsWindowPlacement& placement, bool normalMode);
#endif

#endif
