#ifndef WOW_SOURCE_WORLDCLIENT_WORLDDISPLAYEPOCH_H
#define WOW_SOURCE_WORLDCLIENT_WORLDDISPLAYEPOCH_H
#include <cstdint>
uint64_t WorldDisplayEpoch();
// Safe from lifecycle callbacks; renderer cancellation is performed on its owner.
void InvalidateWorldDisplay();
#endif
