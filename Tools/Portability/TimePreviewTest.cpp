#include "WoW/Source/WorldClient/TimePreview.h"
#include "WoW/Source/WorldClient/NightDarkness.h"
#include "WoW/Source/Game/GameTime.h"
#include <iostream>
#include <limits>
#include <stdexcept>

// Deterministic wall-clock source for the production CGameTime implementation.
static uint32_t nowMs = 1000;
uint32_t OsGetAsyncTimeMs() { return nowMs; }
namespace {
void Check(bool value) { if (!value) throw std::runtime_error("Visual time contract failed"); }
bool Near(float a, float b) { return std::abs(a-b) < .00001f; }
}
int main() {
    try {
        for (const char* text : {"-1", "0", "24", "12.5", " 6 "}) Check(TimePreview::Parse(text).has_value());
        for (const char* text : {"", "-0.5", "-2", "24.01", "nan", "inf", "1e40", "6junk"}) Check(!TimePreview::Parse(text));
        Check(!TimePreview::Parse(nullptr));
        Check(TimePreview::Parse("24") == TimePreview::Parse("0"));
        for (float hour : {0.f, 4.f, 5.f, 6.f, 12.f, 18.f, 20.f, 22.f, 24.f}) {
            float progression = .7f;
            uint32_t minutes = 1008;
            Check(TimePreview::Apply(hour, progression, minutes));
            Check(Near(progression, (hour == 24 ? 0 : hour) / 24));
            Check(minutes == (hour == 24 ? 0 : hour * 60));
            Check(Near(NightDarkness::AmbientScale(progression, 80), NightDarkness::AmbientScale(hour / 24, 80)));
        }
        float progression = .5f;
        uint32_t minutes = 720;
        Check(!TimePreview::Apply(-1, progression, minutes) && progression == .5f && minutes == 720);
        Check(!TimePreview::Apply(std::numeric_limits<float>::quiet_NaN(), progression, minutes));
        Check(TimePreview::Apply(12.5f, progression, minutes) && minutes == 750);

        // The real gameplay clock continues ticking while its visual snapshot is frozen.
        CGameTime clock;
        clock.SetDate(124, 0, 1);
        clock.SetHourAndMinutes(1439);
        clock.m_baseMinuteOfDay = 1439;
        clock.m_minutesPerSecond = 1;
        clock.m_running = 1;
        clock.m_lastTickMs = nowMs;
        clock.m_fracMinutes = .25f;
        clock.m_elapsedMinutes = 0;
        for (int i = 0; i < 3; ++i) {
            const float live = clock.GameTimeGetDayProgression();
            progression = live;
            minutes = clock.GetHourAndMinutes();
            const float ticks = clock.m_fracMinutes;
            const int count = clock.m_minuteTicks;
            Check(TimePreview::Apply(18, progression, minutes));
            Check(progression == .75f && minutes == 1080);
            Check(clock.GameTimeGetDayProgression() == live && clock.m_fracMinutes == ticks && clock.m_minuteTicks == count);
            nowMs += 1000;
            clock.GameTimeUpdate(1);
        }
        Check(clock.GetHourAndMinutes() == 2 && clock.m_minuteTicks == 3 && clock.m_fracMinutes == .25f);
        progression = clock.GameTimeGetDayProgression();
        minutes = clock.GetHourAndMinutes();
        const float resumed = progression;
        Check(!TimePreview::Apply(-1, progression, minutes) && progression == resumed && minutes == 2);
        nowMs += 1000;
        clock.GameTimeUpdate(1);
        Check(clock.GetHourAndMinutes() == 3 && clock.GameTimeGetDayProgression() > resumed);
        std::cout << "Time preview: strict bounds, midnight alias, dawn/dusk, night ambient, production clock ticks and Server Time resumption passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
