// ServoAllocator.cpp

#include "ServoAllocator.h"

namespace ServoAllocator {

// Counts down from channel 7. Once it reaches -1, no more channels
// available.
static int s_nextChannel = 7;

int allocate() {
    if (s_nextChannel < 0) return -1;
    return s_nextChannel--;
}

} // namespace ServoAllocator
