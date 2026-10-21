#ifndef WOW_SOURCE_WORLDCLIENT_WMOLIQUIDHITSNAPSHOT_H
#define WOW_SOURCE_WORLDCLIENT_WMOLIQUIDHITSNAPSHOT_H
#include "Engine/Source/Tempest/C3Segment.h"
#include "Engine/Source/Tempest/CAaBox.h"
#include <cstdint>
#include <memory>
#include <vector>
class CMapObjGroup;
class WmoLiquidHitSnapshot {
    std::vector<C3Vector> vertices;
    std::vector<uint8_t> tiles;
    uint32_t width=0,height=0,stride=0;
    C3Vector corner;
    CAaBox bounds;
    bool typeFlag4=false;
    WmoLiquidHitSnapshot()=default;
public:
    static std::shared_ptr<const WmoLiquidHitSnapshot> Capture(const CMapObjGroup&,bool typeFlag4);
    bool Intersect(const C3Segment&,float& fraction,uint32_t flags) const;
};
#endif
