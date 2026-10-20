#include "Engine/Source/Model2/M2Light.h"
#include <cstddef>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>
void Check(bool b, const char *why) {
    if (!b)
        throw std::runtime_error(why);
}
void SameLegacy(const CM2Lighting &a, const CM2Lighting &b) {
    // Both objects were Initialize()-zeroed, including padding. Provenance may
    // differ, but not a byte of the existing raster lighting state may change.
    Check(std::memcmp(&a, &b, offsetof(CM2Lighting, m_shadowLighting)) == 0,
          "world attribution changed original raster lighting state");
}
int main() {
    try {
        static_assert(std::is_trivially_copyable<M2ShadowLighting>::value,
                      "lighting Initialize uses a zeroed value snapshot");
        CM2Light sun, local, point;
        sun.SetLightType(M2LIGHT_DIRECT);
        sun.SetVisible(1);
        sun.SetDirection(C3Vector(0, 0, -1));
        sun.m_ambColor = C3Vector(.1f, .2f, .3f);
        sun.m_dirColor = C3Vector(.7f, .6f, .5f);
        sun.m_specColor = C3Vector(.2f, .1f, .3f);
        local.SetLightType(M2LIGHT_DIRECT);
        local.SetVisible(1);
        local.SetDirection(C3Vector(1, 0, 0));
        local.m_ambColor = C3Vector(.03f, .04f, .02f);
        local.m_dirColor = C3Vector(.2f, .3f, .1f);
        local.m_specColor = C3Vector(.1f, .2f, .05f);
        point.SetVisible(1);
        point.SetPosition(C3Vector(1, 2, 3));
        point.m_dirColor = C3Vector(.4f, .2f, .3f);
        CAaSphere sphere;
        sphere.c = C3Vector();
        sphere.r = 0;
        CM2Lighting legacy, attributed;
        legacy.Initialize(nullptr, sphere);
        attributed.Initialize(nullptr, sphere);
        legacy.AddLight(&sun);
        attributed.AddWorldDirectionalLight(&sun);
        SameLegacy(legacy, attributed);
        legacy.AddLight(&local);
        attributed.AddLight(&local);
        legacy.AddLight(&point);
        attributed.AddLight(&point);
        SameLegacy(legacy, attributed);
        const auto snapshot = attributed.m_shadowLighting;
        Check(snapshot.worldSourceCount == 1 && !snapshot.conflictingWorldDirections,
              "world source attribution absent/ambiguous");
        Check(snapshot.worldDirection == sun.m_dir && snapshot.worldDiffuse == sun.m_dirColor &&
                  snapshot.worldSpecular == sun.m_specColor,
              "world source was reconstructed from mixed lighting");
        Check(snapshot.unshadowedDirectional.totalColor == local.m_dirColor &&
                  snapshot.unshadowedSpecular == local.m_specColor,
              "local directional source leaked into shadowed source");
        Check(snapshot.ambient == sun.m_ambColor + local.m_ambColor, "ambient source was shadowed or lost");
        Check(attributed.m_lightCount == 1 && attributed.m_lights[0] == &point, "local point light selection changed");
        C3Vector direction, ambient, diffuse, specular;
        legacy.GetSunLight(direction, ambient, diffuse, specular);
        attributed.GetSunLight(direction, ambient, diffuse, specular);
        SameLegacy(legacy, attributed);
        Check(attributed.m_shadowLighting.ambient == snapshot.ambient,
              "legacy SH correction overwrote raw ambient provenance");
        sun.SetDirection(C3Vector(0, 1, 0));
        sun.m_dirColor = C3Vector(.1f, .2f, .3f);
        Check(snapshot.worldDirection == C3Vector(0, 0, -1) && snapshot.worldDiffuse == C3Vector(.7f, .6f, .5f),
              "source snapshot aliases mutable light");
        attributed.AddWorldDirectionalLight(&sun);
        Check(attributed.m_shadowLighting.conflictingWorldDirections,
              "multiple world directions were silently merged into one ray");
        attributed.Reset();
        Check(attributed.m_shadowLighting.worldSourceCount == 0 &&
                  attributed.m_shadowLighting.worldDiffuse == C3Vector() &&
                  attributed.m_shadowLighting.unshadowedDirectional.totalColor == C3Vector(),
              "reset retained stale source provenance");
        attributed.Initialize(nullptr, sphere);
        sun.SetVisible(0);
        attributed.AddWorldDirectionalLight(&sun);
        Check(!attributed.m_shadowLighting.worldSourceCount, "invisible world light admitted");
        attributed.AddWorldDirectionalLight(&point);
        Check(!attributed.m_shadowLighting.worldSourceCount && attributed.m_lightCount == 1,
              "point light relabelled as world directional");
        attributed.Initialize(nullptr, sphere);
        attributed.AddLight(&local);
        Check(!attributed.m_shadowLighting.worldSourceCount,
              "custom/underwater replacement became sun/moon implicitly");
        attributed.Initialize(nullptr, sphere);
        sun.SetVisible(1);
        attributed.AddWorldDirectionalLight(&sun);
        attributed.AddWorldDirectionalLight(&sun);
        Check(attributed.m_shadowLighting.worldSourceCount == 2 &&
                  !attributed.m_shadowLighting.conflictingWorldDirections,
              "same-direction sources were spuriously rejected");
        std::cout << "PASS source-attributed world/ambient/local lighting, byte-identical original aggregate, "
                     "reset/mutation/hidden/conflicting sources\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL lighting sources: " << e.what() << '\n';
        return 1;
    }
}
