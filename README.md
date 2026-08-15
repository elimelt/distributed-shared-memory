# Distributed Shared Memory over TCP

A page-based distributed shared memory engine. Servers export a virtual address space of 4 KiB pages. Clients access it through a local page cache with transparent fault handling, writeback, and prefetching.

Network round-trip latency dominates performance. This is not for latency-critical random access. Consider it when:

- Your working set is large and can be partitioned across nodes.
- Your access pattern is streaming or sequential.
- You run commodity hardware without RDMA, but need memory across nodes.

It is also not for multi-client workloads that need coherence. See [Consistency Model](#consistency-model).

Linux only. The server uses Linux-specific APIs. On other platforms, use the Docker workflow below.

## Architecture

Two layers:

1. **Page API** (`include/dsm_paging.h`, C): page cache with fault handling and clock-based eviction, exported by `libdsm.so`.
2. **Array API** (`python/dsm.py`, Python only): N-dimensional typed arrays with strided indexing, built on the Page API via `ctypes`.

The client keeps a local page cache. A valid cached page is served locally. Otherwise the client fetches the page from its server. Dirty pages are written back on eviction.

### Clustering

Multiple servers form a cluster over a UDP gossip protocol. Each node owns a disjoint page range. Request forwarding is server-side: the client connects to one server and stays cluster-unaware. When a client asks its server for a page another node owns, the server forwards the request to the owner and relays the reply.

Failure detection uses heartbeat timeout aging. Each node tracks when it last heard from each peer. A peer silent for half the timeout moves from `ALIVE` to `SUSPECT`. A peer silent past the full timeout moves to `DEAD`. Defaults: 1000 ms heartbeat, 3000 ms timeout, 500 ms gossip interval.

## Wire Protocol

Client-server communication uses TCP with `TCP_NODELAY`. Every request starts with a fixed 8-byte header:

```
+---------+----------+--------------+
| op (1B) | pad (3B) | page_id (4B) |
+---------+----------+--------------+
```

Operations (`include/dsm_protocol.h`):

- `OP_GET_PAGE (1)`: request a page. The server replies with the raw 4096 bytes of page data.
- `OP_PUT_PAGE (2)`: write a page. 4096 bytes of data follow the header. The server acknowledges the write with an `OP_ACK` header.
- `OP_ACK (3)`: server-to-client acknowledgment of a completed `OP_PUT_PAGE`.

Page ids are `uint32_t`. `UINT32_MAX` is reserved as the no-page sentinel, so the address space can hold up to 2^32 - 1 pages.

### Gossip Protocol (UDP)

Cluster nodes exchange membership via gossip messages. Each message has a header (magic `0x44534D47`, type, node count, sender id, incarnation, client port, page range) plus up to 16 node entries. Each entry carries address, ports, page range, incarnation, and liveness state.

Message types: `PING`, `PONG`, `JOIN`.

## Page Table Implementation

The client maintains:

- **Page table**: maps virtual page numbers to local frames (8 bytes per entry).
- **Frame table**: maps frames to the virtual page they hold.
- **Free list**: stack of available frames.

Eviction uses the **clock algorithm**. A hand sweeps frames and clears reference bits. A page without its reference bit set is evicted. Dirty remote pages are written back first.

### Page Table Entry (8 bytes, packed)

| Field         | Size | Description                     |
|---------------|------|---------------------------------|
| frame_number  | 4B   | Local frame index               |
| valid         | 1B   | Page present in local cache     |
| reference_bit | 1B   | Accessed since last clock sweep |
| dirty         | 1B   | Modified since fetch            |
| location      | 1B   | LOC_LOCAL / LOC_REMOTE          |

## Building

Requires Linux and GCC with C11 support. On macOS or Windows, build and test inside Docker:

```sh
# Build the dev image (gcc + python3)
docker build --target builder -t dsm-build .

# Run any make target against your working tree
docker run --rm -v "$PWD":/w -w /w dsm-build make test
docker run --rm -v "$PWD":/w -w /w dsm-build make python-test

# Build the full runtime image
docker build -t dsm:latest .
```

Make targets (see `make help`):

| Target           | Description                                          |
|------------------|------------------------------------------------------|
| `all`            | Build `dsm-server` and `dsm-client` (default)        |
| `server`         | Build `dsm-server`                                   |
| `client`         | Build `dsm-client`                                   |
| `lib`            | Build the shared library `libdsm.so`                 |
| `python`         | Build `libdsm.so` and print Python usage             |
| `debug`          | Rebuild with debug symbols and sanitizers            |
| `docker`         | Rebuild with portable flags (no `-march=native`)     |
| `perf`           | Run the client under `perf stat`                     |
| `test`           | End-to-end verify test, then unit tests              |
| `unit`           | Unit tests (paging, gossip)                          |
| `python-test`    | Python binding end-to-end test                       |
| `cluster-test`   | 2-node cluster test                                  |
| `docker-build`   | Build the Docker image `dsm:latest`                  |
| `docker-run`     | Start the 3-node cluster with `docker compose`       |
| `cluster-docker` | Run the cluster benchmark via `docker compose`       |
| `clean`          | Remove built files                                   |
| `install`        | Install binaries to `PREFIX/bin` (default `/usr/local`) |
| `help`           | List targets                                         |

## Usage

### Single Server

```sh
./dsm-server -p 9999 -n 4096          # serve 4096 virtual pages
./dsm-client -H localhost -p 9999 -i 10000   # run the benchmark
./dsm-client -H localhost --verify           # round-trip verification
```

The client runs a synthetic benchmark by default. `-h` on either binary lists all flags.

### Cluster (3 nodes)

```sh
# Node 1: owns pages 0-1023
./dsm-server -p 9001 -c 10001 -r 0:1024 -n 3072

# Node 2: owns pages 1024-2047, joins via node 1
./dsm-server -p 9002 -c 10002 -r 1024:2048 -n 3072 -s localhost -S 10001

# Node 3: owns pages 2048-3071
./dsm-server -p 9003 -c 10003 -r 2048:3072 -n 3072 -s localhost -S 10001
```

Clients connect to any one node. The server forwards requests for pages it does not own.

Or run the 3-node cluster in containers: `make docker-run` (uses `docker-compose.yaml`).

## C API

The Page API lives in `include/dsm_paging.h`. You provide a connected TCP socket (see `connect(2)`; the reference client is `src/dsm_client.c`).

```c
#include <stdio.h>
#include <string.h>
#include "dsm_paging.h"

void example(int server_fd)  /* TCP socket connected to dsm-server */
{
    /* 256 local frames caching a 4096-page virtual space */
    dsm_context_t *ctx = dsm_create_context(256, 4096);
    dsm_context_set_socket(ctx, server_fd);

    /* Write to page 7 */
    char *p = dsm_access_page(ctx, (uint32_t)7, 1 /* write */);
    strcpy(p, "hello");

    /* Hint that pages 8..11 will be needed soon */
    dsm_prefetch_pages(ctx, 8, 4);

    /* Read page 7 back */
    p = dsm_access_page(ctx, (uint32_t)7, 0);
    printf("%s\n", p);

    uint64_t hits, fetches, evictions;
    dsm_context_get_stats(ctx, &hits, &fetches, &evictions);
    printf("hits=%llu fetches=%llu evictions=%llu\n",
           (unsigned long long)hits, (unsigned long long)fetches,
           (unsigned long long)evictions);

    dsm_destroy_context(ctx);
}
```

For raw page RPCs without the cache, `include/dsm_protocol.h` exports `dsm_rpc_get(fd, page_id, buf)` and `dsm_rpc_put(fd, page_id, buf)`.

## Python API

`python/dsm.py` wraps `libdsm.so` (build it with `make lib`; set `DSM_LIB` if the library is not at the repo root). `Context` manages the connection. `Array` allocates pages from the context, so two arrays never overlap.

```python
import dsm

with dsm.Context("localhost", 9999, local_pages=256, virtual_pages=4096) as ctx:
    a = dsm.Array(ctx, shape=(1000, 1000), dtype=dsm.float64)
    b = dsm.Array(ctx, shape=(100,), dtype=dsm.int32)   # separate pages from a

    a[50, 100] = 3.14
    print(a[50, 100])
    print(ctx.stats)   # property: {'local_hits': ..., 'remote_fetches': ..., 'evictions': ...}
```

`dsm.to_numpy(arr)` and `dsm.from_numpy(ctx, np_arr)` convert to and from NumPy arrays.

## Configuration

Both binaries read defaults, then environment variables, then CLI flags. CLI flags win. All numeric values are range-checked: a bad CLI value exits with usage, a bad environment value logs a warning and keeps the default.

Environment variables (parsed in `src/dsm_config.c`):

| Variable | Used by | Meaning |
|----------|---------|---------|
| `DSM_HOST` | client | Server hostname |
| `DSM_PORT` | both | Server TCP port |
| `DSM_BIND_ADDR` | server | Bind address (default `0.0.0.0`) |
| `DSM_NUM_PAGES` | client | Local cache size in pages |
| `DSM_NUM_VIRTUAL_PAGES` | both | Total virtual pages |
| `DSM_NUM_ITERATIONS` | client | Benchmark iterations |
| `DSM_LOCALITY_PERCENT` | client | Benchmark locality (0-100) |
| `DSM_WRITE_PERCENT` | client | Benchmark write ratio (0-100) |
| `DSM_VERBOSE` | both | Set to `1` for verbose logs |
| `DSM_VERIFY` | client | Set to `1` for verify mode |
| `DSM_SEED_ADDR` | server | Seed node address to join |
| `DSM_SEED_PORT` | server | Seed node cluster port |
| `DSM_NODE_ID` | server | Node identifier |
| `DSM_CLUSTER_PORT` | server | This node's cluster (UDP) port |
| `DSM_HEARTBEAT_MS` | server | Heartbeat interval |
| `DSM_GOSSIP_MS` | server | Gossip interval |
| `DSM_TIMEOUT_MS` | server | Failure detection timeout |
| `DSM_PAGE_RANGE_START` | server | First page this node owns |
| `DSM_PAGE_RANGE_END` | server | One past the last page this node owns |

## Testing

- `make test`: starts a server, runs the client in verify mode (write, evict, writeback, refetch), then runs the unit tests.
- `make unit`: paging and gossip unit tests. No server needed.
- `make python-test`: end-to-end test of the Python bindings against a live server.
- `make cluster-test`: 2-node cluster benchmark with server-side forwarding.

## Consistency Model

There is **no coherence protocol**. Each client operates on its local cache independently. If multiple clients write the same page:

- Last writer wins, based on writeback order.
- No read-your-writes guarantee across clients.
- No atomicity for multi-page operations.

If you need stronger consistency, add locking at the application layer, or use this system for read-mostly workloads with partitioned writes.

## License

MIT
