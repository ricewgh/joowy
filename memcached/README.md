# Concurrent memcached server

This is a small, in-memory C++17 cache server for Linux.
Clients connect over TCP on **127.0.0.1**; data is lost when the server exits.

- **Concurrency:** connections are served by a pool of epoll worker threads
  (one per CPU, 2 to 16), so many idle or slow clients do not each hold a thread.
- **Cache locking:** the cache is split into 32 shards, each protected by a
  writer-preferring reader/writer lock, so readers run in parallel and a steady
  read load cannot starve writers. Operations on different shards never contend.
  Values are immutable and reference counted, so a read copies a pointer under the
  lock rather than the value. Each individual operation is atomic; a sequence of
  operations is not a transaction.
- **Memory:** the cache is capped (256 MB by default). When a shard is full, its
  least recently written entries are evicted. A background sweep removes expired
  entries that nobody reads.
- **Robustness:** idle connections are closed after 60 seconds, and a client that
  stops reading its responses cannot make the server buffer unbounded output.

## Build and test

Requirements: a C++17 compiler, CMake 3.16 or newer, Linux sockets, and Python 3
for the network integration tests.

```sh
cmake -S memcached -B memcached/build
cmake --build memcached/build
ctest --test-dir memcached/build --output-on-failure
```

## Run and try

In one terminal:

```sh
./memcached/build/memcached_server        # default port 11211
```

Optional arguments, in order: `[port] [cache megabytes] [idle timeout seconds]`.
A cache size of `0` means unlimited. For example, `memcached_server 11212 64 30`
listens on port 11212 with a 64 MB cache and a 30 second idle timeout.

In another terminal:

```sh
./memcached/build/memcached_cli           # pass a port to either program if needed
```

Enter `set greeting "hello world"`, `get greeting`, `delete greeting`, and
`quit`. Values containing spaces should be double-quoted; backslash escapes
quotes and backslashes inside quoted values. The CLI uses flags `0` and no
expiration. Stop the server with Ctrl+C.

## Protocol and limits

The server implements the memcached **text protocol subset** `get` (including
multiple keys), `set`, `delete`, and `quit`, including `noreply` for `set` and
`delete`. Standard text-protocol clients can use these commands. Other
memcached commands and the binary protocol are not supported. For example:

```text
set greeting 0 0 11
hello world
get greeting
delete greeting
```

On the wire, each command and value is terminated by CRLF; `set` specifies the
value's byte length. Values may contain arbitrary bytes, including CRLF and
NUL. Keys follow memcached's text-protocol restrictions: 1–250 bytes, without
spaces or control characters. Flags are unsigned 32-bit integers. Expiration
is zero for no expiration, a relative number of seconds up to 30 days, or a
Unix timestamp above 30 days. Relative expirations use a monotonic clock, so
changing the system time does not affect them. Expired entries are removed when
accessed and by the background sweep.
Values are limited to 1 MiB and command lines to 8192 bytes. A `get` may list at
most 100 keys. Oversized values or malformed `set` requests result in an error and
connection closure. At most 4096 concurrent client connections are accepted (fewer
if the process file descriptor limit is lower); further clients receive
`SERVER_ERROR too many connections`.

Limitations: the cache size limit is approximate (it counts keys, values and a fixed
per-entry overhead), eviction is by write order rather than true LRU, and a single
`get` response is built in memory, so many large values in one request use
proportionally more memory. There is no authentication, persistence, TLS, or
remote bind option. The writer-preferring lock needs glibc; on other C libraries
it falls back to `std::shared_mutex`, which can let readers delay writers.
