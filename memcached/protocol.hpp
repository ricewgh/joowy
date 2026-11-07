#ifndef JOOWY_MEMCACHED_PROTOCOL_HPP
#define JOOWY_MEMCACHED_PROTOCOL_HPP

#include "cache.hpp"

#include <cstddef>
#include <string>

enum class ProcessStatus {
    NeedInput,   // all complete commands were handled; the rest is a partial command
    OutputFull,  // stopped early because output reached the limit; call again once it is sent
    Close,       // the connection must be closed after the output is sent
};

// Executes the complete commands at the start of `input`, removes them from it, and appends
// the responses to `output`. Never blocks, so it is usable from an event loop.
ProcessStatus process_commands(Cache& cache, std::string& input, std::string& output,
                               std::size_t output_limit);

#endif
