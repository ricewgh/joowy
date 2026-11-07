#ifndef JOOWY_MEMCACHED_PROTOCOL_HPP
#define JOOWY_MEMCACHED_PROTOCOL_HPP

#include "cache.hpp"

void serve_client(int socket, Cache& cache);

#endif
