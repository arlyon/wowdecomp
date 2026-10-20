#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/ConsoleBase/Types.h"
#include "WoW/Source/WorldClient/WorldParam.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        // Real CVar storage and production callbacks, without world/device startup.
        auto* aa = CVar::Register("sceneRegressionAA", "", 1, "0", CWorldParam::NativeAACallback, CATEGORY_GRAPHICS, false, nullptr, false);
        auto* scale = CVar::Register("sceneRegressionScale", "", 1, "1", CWorldParam::NativeSceneScaleCallback, CATEGORY_GRAPHICS, false, nullptr, false);
        aa->Set("1", true, false, false, true);
        Check(aa->m_intValue == 1, "AA enable rejected");
        for (const char* invalid : {"", "-1", "2", "1x", "nan", "inf", "1.0"}) {
            aa->Set(invalid, true, false, false, true);
            Check(aa->m_intValue == 1 && std::string(aa->m_stringValue.m_str) == "1", "Invalid AA value stored");
        }
        aa->Set("0", true, false, false, true);
        Check(aa->m_intValue == 0, "AA disable rejected");
        for (const char* valid : {"0.5", "0.75", "1"}) {
            scale->Set(valid, true, false, false, true);
            Check(std::string(scale->m_stringValue.m_str) == valid, "Scene scale boundary rejected");
        }
        scale->Set("0.75", true, false, false, true);
        for (const char* invalid : {"", "0", "0.49", "1.01", "-1", "nan", "inf", "0.75x", "1e999"}) {
            scale->Set(invalid, true, false, false, true);
            Check(std::abs(scale->m_floatValue - .75f) < 1e-6f && std::string(scale->m_stringValue.m_str) == "0.75", "Invalid scene scale replaced saved value");
        }
        Check(!CWorldParam::NativeAACallback(nullptr, nullptr, nullptr, nullptr), "Null AA accepted");
        Check(!CWorldParam::NativeSceneScaleCallback(nullptr, nullptr, nullptr, nullptr), "Null scale accepted");
        Check(CVar::m_needsSave, "Scene settings did not request persistence");
        std::cout << "PASS production AA/scene-scale callbacks and CVar persistence/rejection\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
