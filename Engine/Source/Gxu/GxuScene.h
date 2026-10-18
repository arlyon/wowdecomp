#ifndef ENGINE_SOURCE_GXU_GXUSCENE_H
#define ENGINE_SOURCE_GXU_GXUSCENE_H

#if defined(_WIN32)
#define GXU_SCENE_INTERFACE
#else
// This is an interface: no base implementations or base instances exist.
#define GXU_SCENE_INTERFACE = 0
#endif
#line 4
#include <cstdint>
#include "Engine/Source/Storm/H/stpl.h"

class GxuSceneObject {
    public:
    virtual void Unk00() GXU_SCENE_INTERFACE;
    virtual void Release() GXU_SCENE_INTERFACE;
    virtual bool IsAlive() GXU_SCENE_INTERFACE;
    virtual void Update(float elapsed) GXU_SCENE_INTERFACE;
    virtual void Render() GXU_SCENE_INTERFACE;
};

class GxuScene {
    public:
    void Render();
    /* 0004 */ TSGrowableArray<GxuSceneObject*> m_objects;
    /* 0014 */ uint32_t m_lastUpdateTime;

    virtual ~GxuScene();
    void Clear();
    void Update(uint32_t time);
};

#endif
