#include "Engine/Source/SteelseriesMouse/HidManager.h"
#include "Engine/Source/SteelseriesMouse/WowMouse.h"
#include "WoW/Source/Ui/UILCD/LCD.h"
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <memory>

// Always evaluate behavioral checks, including in Release builds with NDEBUG.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        std::exit(EXIT_FAILURE); \
    } \
} while (false)

int main() {
    for (int cycle = 0; cycle != 3; ++cycle) {
        std::unique_ptr<HidManager> hid(HidManager::Create());
        CHECK(hid);
        hid->SetWindow(nullptr);
        errno = 0;
        CHECK(!hid->EnumerateDevices() && errno == ENOTSUP);
        CHECK(hid->GetDeviceCount() == 0);
        unsigned char report[] = {0, 1, 2};
        for (int attempt = 0; attempt != 2; ++attempt) {
            errno = 0;
            CHECK(!hid->Write(report, sizeof(report)) && errno == ENOTSUP);
            errno = 0;
            CHECK(!hid->WriteWait(nullptr, 0) && errno == ENOTSUP);
            errno = 0;
            CHECK(!hid->SetFeature(report, sizeof(report)) && errno == ENOTSUP);
            CHECK(hid->GetDeviceCount() == 0);
        }
        // No SDL/OS GUI window is created or linked: native ownership is safe
        // even before the ordinary input subsystem has created a window.
        WowMouse mouse;
        CHECK(mouse.m_hidManager && mouse.IsConnected() == 0);

        CGLCD::Shutdown();
        errno = 0;
        CGLCD::Initialize();
        CHECK(errno == ENOTSUP);
        CHECK(!CGLCD::m_lcd && !CGLCD::initComplete);
        CHECK(!CGLCD::m_player && !CGLCD::m_inWorld);
        errno = 0;
        CHECK(!CGLCD::LoadLayout() && errno == ENOTSUP);
        CHECK(!CGLCD::LoadLCDMap(0, nullptr, nullptr, 0));
        CHECK(!CGLCD::LoadLCDText(0, nullptr, nullptr, 0));
        CHECK(!CGLCD::LoadLCDBitmap(0, nullptr, nullptr));
        CHECK(!CGLCD::LoadLCDElement(0, nullptr, nullptr, 0));
        CHECK(!CGLCD::LoadGlobals(nullptr, nullptr));
        CHECK(!CGLCD::GeneratePOIs(nullptr));
        CHECK(!CGLCD::LoadScreen(nullptr, nullptr));
        CHECK(!CGLCD::GetHandle("missing"));
        CGLCD::Login();
        CGLCD::EnterWorld();
        CGLCD::PostInit();
        CGLCD::CheckInput();
        CGLCD::OnWhisper("local fixture");
        CGLCD::OnBattlefieldReady(nullptr);
        CGLCD::OnBattlefieldActive();
        CGLCD::ShowBattlefieldScreen();
        CGLCD::ClearWaitQueueScreen();
        CGLCD::HideBattlefieldAlert();
        CGLCD::IdleDisconnectWarning();
        CGLCD::AuctionOutbid("local fixture");
        CGLCD::AuctionExpired("local fixture");
        CGLCD::AuctionSold("local fixture");
        CGLCD::AuctionWon("local fixture");
        CGLCD::PushAlert("local fixture", 1);
        CGLCD::UpdateMapPositions();
        CGLCD::UpdateBattlefieldStats();
        CGLCD::SetScreen(1);
        CGLCD::SetStat("fixture", 1);
        CGLCD::SetStat("fixture", 1.0f, true);
        CGLCD::SetStat("fixture", 1, 2);
        CGLCD::SetStat("fixture", "text");
        CGLCD::SetHandleText("fixture", "text");
        CGLCD::DisplayMeleeStats();
        CGLCD::DisplayRangedStats();
        CGLCD::DisplayDefenseStats();
        CGLCD::DisplaySpellStats();
        CGLCD::SetAlertText();
        CHECK(CGLCD::Update(nullptr, nullptr) == 1); // Dispatcher result only.
        CHECK(!CGLCD::m_lcd && !CGLCD::initComplete);
        CHECK(!CGLCD::m_player && !CGLCD::m_inWorld);
        CHECK(CGLCD::m_handles.m_fulllist.IsEmpty());
        CHECK(CGLCD::m_mapData.IsEmpty() && CGLCD::m_POIIcons.IsEmpty());
        CGLCD::Logout();
        CGLCD::LeaveWorld();
        CGLCD::Shutdown();
        CGLCD::Shutdown();
    }

    const char* screens[] = {"WoWTitleScreen", "PlayerInfoScreen",
        "BattlefieldAlertScreen", "BattlefieldStatusScreen", "NotInBattlefieldScreen",
        "WaitQueueScreen", "ChatAlertScreen", "ArathiBasinMapScreen", "WarsongMapScreen",
        "EyeOfTheStormMapScreen", "StrandOfTheAncientsMapScreen", "AlteracValleyMapScreen",
        "IsleOfConquestMapScreen"};
    for (int i = 0; i != 13; ++i) CHECK(CGLCD::GetScreenFromName(screens[i]) == i);
    CHECK(CGLCD::GetScreenFromName("wowtitlescreen") == 0);
    CHECK(CGLCD::GetScreenFromName("unknown") == -1);
    {
        LCDHANDLE handle;
        handle.SetText("generated local text");
        CHECK(std::strcmp(handle.m_text.m_str, "generated local text") == 0);
    }
    CStringManager::DestroyManager();
}
