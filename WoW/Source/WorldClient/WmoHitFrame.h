#ifndef WOW_SOURCE_WORLDCLIENT_WMOHITFRAME_H
#define WOW_SOURCE_WORLDCLIENT_WMOHITFRAME_H
#include "WmoGeometryHitSnapshot.h"
#include "WmoLiquidHitSnapshot.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include <cstdint>
#include <memory>
#include <vector>
// WMO-only portion of an owned interaction generation. Never runs model callbacks.
class WmoHitFrame {
public:
    struct Entry {
        CAaBox definitionBounds,groupBounds,localBounds;
        C44Matrix inverse;
        uint32_t definitionFlags=0,groupKey=0;
        uint64_t guid=0;
        std::shared_ptr<const WmoGeometryHitSnapshot> geometry;
        std::shared_ptr<const WmoLiquidHitSnapshot> liquid;
    };
    struct Candidate {size_t entry;float fraction;};
    struct Hit {uint64_t guid=0;uint32_t groupKey=0;int32_t face=-1;};
    static std::shared_ptr<const WmoHitFrame> Capture();
    explicit WmoHitFrame(std::vector<Entry>);
    std::vector<Candidate> Candidates(const C3Segment&,uint32_t definitionIgnoreFlags) const;
    bool Intersect(const C3Segment&,float& fraction,uint32_t flags,uint32_t definitionIgnoreFlags,Hit&) const;
    size_t Count() const{return entries.size();}
    uint32_t GroupKey(size_t entry) const{return entries.at(entry).groupKey;}
    std::vector<uint64_t> CollisionGuids() const {std::vector<uint64_t> result;for(const auto& e:entries)if(e.guid)result.push_back(e.guid);return result;}
private:
    std::vector<Entry> entries;
};
#endif
