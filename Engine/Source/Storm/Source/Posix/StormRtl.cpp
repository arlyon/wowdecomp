#include "Engine/Source/Storm/H/storm.h"

void StormRtlInitialize() {
    // Native SMem uses the already-live libc allocator, with no separate
    // initialization phase (the shared SMemInitialize is empty as well).
    SLogInitialize();
}
