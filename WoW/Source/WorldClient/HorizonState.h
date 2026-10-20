#ifndef WOW_WORLDCLIENT_HORIZONSTATE_H
#define WOW_WORLDCLIENT_HORIZONSTATE_H

#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"

namespace HorizonState {
// WDL has only position/colour: inherited material programs contain near-scene
// projection constants, and inherited alpha textures can punch holes in it.
inline void Bind(CGxDevice& device) {
    device.RsSet(GxRs_VertexShader, static_cast<void*>(nullptr));
    device.RsSet(GxRs_PixelShader, static_cast<void*>(nullptr));
    device.RsSet(GxRs_Texture0, static_cast<void*>(nullptr));
    device.RsSet(GxRs_AlphaRef, 0);
    device.RsSet(GxRs_BlendingMode, 0);
    device.RsSet(GxRs_Lighting, 0);
}
}
#endif
