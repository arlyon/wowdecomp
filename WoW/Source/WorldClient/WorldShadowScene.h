#ifndef WOW_WORLD_SHADOW_SCENE_H
#define WOW_WORLD_SHADOW_SCENE_H
#include "Engine/Source/Gx/ShadowScene.h"
#include <memory>
class C3Vector;
class C44Matrix;
std::shared_ptr<const GxShadow::Frame> CollectWorldShadowScene(const C3Vector& camera,const C44Matrix& projection);
#endif
