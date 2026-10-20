#include "LiquidEnvironment.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/LiquidSystem/Liquid.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/Map.h"
#include "WoW/Source/WorldClient/MapLight.h"
#include "WoW/Source/WorldClient/World.h"

// STATIC: 0x00D31A00
CDataAllocator Liquid::s_clientEnvironmentAllocator(sizeof(Liquid::CClientEnvironment), 0x10);

// VERIFY: 6d5d98ec auto
// OFFSET: 0x007D5100
void Liquid::ClearClientEnvironments() {
    Liquid::s_clientEnvironmentAllocator.Clear(".?AVCClientEnvironment@Liquid@@", SLOG_OBJECT, 1);
}

// VERIFY: 5ea0339f auto
// OFFSET: 0x007D4F10
void Liquid::CClientEnvironment::SetInteriorFog(int32_t interiorFog) {
    this->m_interiorFog = interiorFog;
}

// VERIFY: 2f7c754e auto
// OFFSET: 0x007D4F20
void Liquid::CClientEnvironment::Release() {
    this->~CClientEnvironment();
    Liquid::s_clientEnvironmentAllocator.PutData(this, nullptr, 0);
}

// VERIFY: 2a7d5c2c manual
// OFFSET: 0x007D4F40
void Liquid::CClientEnvironment::SelectLights(CM2Lighting* lighting) {
    DayNight::DNInfo* dayNight = DayNight::GetInfo();

    C3Vector fogColor;
    float fogStart;
    float fogEnd;
    float fogDensity;

    if (this->m_interiorFog) {
        fogColor = C3Vector(dayNight->m_fogInterior.m_color);
        fogDensity = dayNight->m_fogInterior.m_density;
        fogEnd = dayNight->m_fogInterior.m_end;
        fogStart = dayNight->m_fogInterior.m_start;
    } else {
        fogColor = C3Vector(dayNight->m_fog.m_color);
        fogDensity = dayNight->m_fog.m_density;
        fogEnd = dayNight->m_fog.m_end;
        fogStart = dayNight->m_fog.m_start;
    }

    lighting->SetFog(fogColor, fogStart, fogEnd, fogDensity);

    if (this->m_customLight) {
        static CM2Light s_light;
        static int32_t s_lightInitialized;

        if (!s_lightInitialized) {
            s_light.SetLightType(M2LIGHT_DIRECT);
            s_light.m_constantAttenuation = 1.0f;
            s_light.m_linearAttenuation = 0.0f;
            s_light.m_quadraticAttenuation = 0.0f;
            s_light.SetDirection(C3Vector(0.0f, 0.0f, -1.0f));
            s_light.m_dirColor = C3Vector(1.0f, 1.0f, 1.0f);
            s_light.SetVisible(1);
            s_lightInitialized = 1;
        }

        lighting->AddLight(&s_light);
    } else {
        lighting->AddWorldDirectionalLight(&CMap::s_mapLight->m_light);
    }

    World::s_m2Scene->SelectLights(lighting);
}

// VERIFY: 8ab34ec9 auto
// OFFSET: 0x007D5120
Liquid::CClientEnvironment* Liquid::CClientEnvironment::Create(int32_t customLight) {
    void* mem = Liquid::s_clientEnvironmentAllocator.GetData(0, ".?AVCClientEnvironment@Liquid@@", SLOG_OBJECT);
    Liquid::CClientEnvironment* environment = mem ? new (mem) Liquid::CClientEnvironment() : nullptr;
    environment->m_interiorFog = 0;
    environment->m_customLight = customLight;

    return environment;
}
