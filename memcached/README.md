# Concurrent memcached server

This is a small, in-memory C++17 cache server for Linux.
Clients connect over TCP on **127.0.0.1**; data is lost when the server exits.
Multiple client connections are handled concurrently, and cache operations are
protected by a reader/writer lock. Each individual operation is atomic; a
sequence of operations is not a transaction.

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
Unix timestamp above 30 days. Expired entries are removed when accessed.
Values are limited to 1 MiB and command lines to 8192 bytes. Oversized values
or malformed `set` requests result in an error and connection closure. At most
256 concurrent client connections are accepted. There is no eviction policy,
authentication, persistence, TLS, or remote bind option.
