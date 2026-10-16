#include "fmod_os_net.h"
// This native port supports local audio only. DSP networking can call these
// even without FMOD_SUPPORT_NET; never create sockets or resolve hostnames.
FMOD_RESULT FMOD_OS_Net_Init() { return FMOD_ERR_UNSUPPORTED; }
FMOD_RESULT FMOD_OS_Net_Shutdown() { return FMOD_ERR_UNSUPPORTED; }
FMOD_RESULT FMOD_OS_Net_Connect(const char *, unsigned short, void **h)
{ if (h) *h = nullptr; return FMOD_ERR_UNSUPPORTED; }
FMOD_RESULT FMOD_OS_Net_Listen(unsigned short, void **h)
{ if (h) *h = nullptr; return FMOD_ERR_UNSUPPORTED; }
FMOD_RESULT FMOD_OS_Net_Accept(const void *, void **h)
{ if (h) *h = nullptr; return FMOD_ERR_UNSUPPORTED; }
FMOD_RESULT FMOD_OS_Net_Close(const void *) { return FMOD_ERR_UNSUPPORTED; }
FMOD_RESULT FMOD_OS_Net_Write(const void *, const char *, unsigned int, unsigned int *n)
{ if (n) *n = 0; return FMOD_ERR_UNSUPPORTED; }
FMOD_RESULT FMOD_OS_Net_Read(const void *, char *, unsigned int, unsigned int *n)
{ if (n) *n = 0; return FMOD_ERR_UNSUPPORTED; }
FMOD_RESULT FMOD_OS_Net_ReadLine(const void *, char *, unsigned int) { return FMOD_ERR_UNSUPPORTED; }
