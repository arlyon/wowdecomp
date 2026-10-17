#if !defined(_WIN32)

#include "WoW/Source/Ui/UILCD/LCD.h"
#include <cerrno>

// No Logitech SDK/backend is installed: null LCD and incomplete init are the
// existing unavailable state. No device object, idle handler, or input is created.
CEzLcd* CGLCD::m_lcd = nullptr;
int32_t CGLCD::m_chatAlertLines = 0;
uint8_t CGLCD::initComplete = 0;
CGPlayer_C* CGLCD::m_player = nullptr;
uint8_t CGLCD::m_inWorld = 0;
TSHashTable<LCDHANDLE, HASHKEY_CONSTSTRI> CGLCD::m_handles;
TSList<MAPDATA, TSGetLink<MAPDATA> > CGLCD::m_mapData;
TSList<POIIcon, TSGetLink<POIIcon> > CGLCD::m_POIIcons;

void CGLCD::Initialize() {
    CGLCD::Shutdown();
    errno = ENOTSUP;
}

void CGLCD::Shutdown() {
    CGLCD::m_lcd = nullptr;
    CGLCD::initComplete = 0;
    CGLCD::m_player = nullptr;
    CGLCD::m_inWorld = 0;
    CGLCD::m_chatAlertLines = 0;
}

// 1 is the event dispatcher's continue/handled result, not hardware I/O success.
int32_t CGLCD::Update(const void*, void*) { return 1; }

void CGLCD::Logout() {}
void CGLCD::ClearWaitQueueScreen() {}
void CGLCD::IdleDisconnectWarning() {}
void CGLCD::SetScreen(int32_t) {}
void CGLCD::ShowBattlefieldScreen() {}
void CGLCD::HideBattlefieldAlert() {}
void CGLCD::OnBattlefieldReady(const BattlefieldStatus*) {}
void CGLCD::OnBattlefieldActive() {}
void CGLCD::UpdateBattlefieldStats() {}
void CGLCD::OnWhisper(const char*) {}
void CGLCD::AuctionOutbid(const char*) {}
void CGLCD::AuctionExpired(const char*) {}
void CGLCD::AuctionSold(const char*) {}
void CGLCD::AuctionWon(const char*) {}
void CGLCD::PushAlert(const char*, int32_t) {}
void CGLCD::Login() {}
void* CGLCD::GetHandle(const char*) { return nullptr; }
void CGLCD::CheckInput() {}
void CGLCD::UpdateMapPositions() {}
void CGLCD::SetStat(const char*, int32_t) {}
void CGLCD::SetStat(const char*, float, bool) {}
void CGLCD::SetStat(const char*, int32_t, int32_t) {}
void CGLCD::SetStat(const char*, const char*) {}
void CGLCD::SetHandleText(const char*, const char*) {}
void CGLCD::DisplayMeleeStats() {}
void CGLCD::DisplayRangedStats() {}
void CGLCD::DisplayDefenseStats() {}
void CGLCD::DisplaySpellStats() {}
void CGLCD::SetAlertText() {}
void CGLCD::PostInit() {}
bool CGLCD::LoadLCDMap(int32_t, XMLNode*, CStatus*, int32_t) { errno = ENOTSUP; return false; }
bool CGLCD::LoadLCDText(int32_t, XMLNode*, CStatus*, int32_t) { errno = ENOTSUP; return false; }
bool CGLCD::LoadLCDBitmap(int32_t, XMLNode*, CStatus*) { errno = ENOTSUP; return false; }
bool CGLCD::LoadLCDElement(int32_t, XMLNode*, CStatus*, int32_t) { errno = ENOTSUP; return false; }
bool CGLCD::LoadGlobals(XMLNode*, CStatus*) { errno = ENOTSUP; return false; }
bool CGLCD::GeneratePOIs(XMLNode*) { errno = ENOTSUP; return false; }
bool CGLCD::LoadScreen(XMLNode*, CStatus*) { errno = ENOTSUP; return false; }
bool CGLCD::LoadLayout() { errno = ENOTSUP; return false; }
void CGLCD::EnterWorld() {}
void CGLCD::LeaveWorld() {}

#endif
