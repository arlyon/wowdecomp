#ifndef WOW_SOURCE_WORLDCLIENT_WMOGEOMETRYHITSNAPSHOT_H
#define WOW_SOURCE_WORLDCLIENT_WMOGEOMETRYHITSNAPSHOT_H
#include "WoW/Common/Types.h"
#include "WoW/Source/WorldClient/Types.h"
#include "Engine/Source/Tempest/C3Segment.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include <memory>
#include <vector>
class CMapObjGroup;
// Owned original WMO BSP/face/material collision, not a raster approximation.
// Group liquid and instance transforms are separate producers.
class WmoGeometryHitSnapshot {
    std::vector<CAaBspNode> nodes;
    std::vector<uint16_t> leafFaces,indices;
    std::vector<C3Vector> vertices;
    std::vector<SMOPoly> faces;
    std::vector<uint32_t> blendModes;
    CAaBox bounds;
    WmoGeometryHitSnapshot()=default;
public:
    static std::shared_ptr<const WmoGeometryHitSnapshot> Capture(const CMapObjGroup&);
    bool Intersect(const C3Segment&,float& fraction,uint16_t ignoreFlags,int32_t& face) const;
};
#endif
