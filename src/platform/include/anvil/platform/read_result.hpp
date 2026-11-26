#pragma once

#include <cstddef>

namespace anvil::platform {

// What a read that waits with a timeout gives.
struct ReadResult {
    std::size_t bytes = 0;
    // True once the peer has closed its end and all data has been read.
    bool closed = false;
};

} // namespace anvil::platform
