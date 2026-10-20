#ifndef MODEL2_SHADOW_MESH_H
#define MODEL2_SHADOW_MESH_H
#include "Engine/Source/Gx/ShadowScene.h"
class CM2Model;
class CM2Scene;
// Original animator preparation; never called by AS/ray submission.
void PrepareM2ShadowPoses(CM2Scene& scene);
// Read-only snapshot of an already initialised, current original pose/profile.
GxShadow::FrameResult BuildM2ShadowMesh(const CM2Model& model,const CM2Scene& scene,uint32_t batch,GxShadow::Mesh& out);
#endif
