#include "WorldDisplayEpoch.h"
#include <atomic>
namespace {std::atomic<uint64_t> epoch{1};}
uint64_t WorldDisplayEpoch(){return epoch.load(std::memory_order_acquire);}
void InvalidateWorldDisplay(){epoch.fetch_add(1,std::memory_order_acq_rel);}
