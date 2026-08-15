# Distributed Shared Memory over TCP

A page-based distributed shared memory engine that presents a unified virtual address space across multiple machines. Clients access remote memory through a local page cache with transparent fault handling, writeback, and prefetching.

This is not intended for workloads where low-latency random access is critical. Network round-trip latency dominates performance. Rather, you should consider this for scenarios where:

- You have a relatively large working set that can be partitioned across nodes
- You have a streaming access pattern 
- You are running commodity hardware and don't have access to something like Infiniband that supports low-latency RDMA, but still need to allocate memory across nodes

Similarly, this is not intended for multi-client workloads where coherence is required. See [Consistency Model](#consistency-model) for more details. 

## Architecture

The system consists of three layers:

1. **Page API** (`dsm_paging.h`) -- Low-level page fault handling with clock-based eviction
2. **Region API** (`dsm_region.h`) -- Contiguous byte-addressed memory regions
3. **Array API** (`dsm_array.h`) -- Multi-dimensional typed arrays with strided indexing

Clients maintain a local page cache. On access, if the page is present and valid, the access completes locally. Otherwise, the client fetches the page from the server (or forwards the request to the owning node in a cluster). Dirty pages are written back on eviction.

### Clustering

Multiple servers form a cluster using a gossip protocol over UDP. Each node owns a disjoint range of pages. When a client requests a page not owned by its connected server, the request is forwarded to the correct owner. Node membership is disseminated via periodic heartbeats and SWIM-style failure detection.

## Wire Protocol

All communication uses TCP with `TCP_NODELAY`. Messages are fixed-size for predictable parsing.

### RPC Header (8 bytes)

```
+---------+----------+-------------+
| op (1B) | pad (3B) | page_id(4B) |
+---------+----------+-------------+
```

**Operations:**
- `OP_GET_PAGE (1)` -- Request page data; server responds with 4096 bytes
- `OP_PUT_PAGE (2)` -- Write page; followed by 4096 bytes of data
- `OP_ACK (3)` -- Acknowledge write completion

### Gossip Protocol (UDP)

Cluster nodes communicate membership via gossip messages:

```
+-------------+------+------------+-----------------+---------------------+
| magic (4B)  |type  | node_count | sender_id (16B) | sender_range (8B)   |
| 0x44534D47  |(1B)  | (1B)       |                 | start:end           |
+-------------+------+------------+-----------------+---------------------+
```

Message types: `PING`, `PONG`, `JOIN`, `JOIN_ACK`, `SYNC`, `LEAVE`

Each message includes up to 16 node entries with address, port, page range, incarnation number, and liveness state.

## Page Table Implementation

The client maintains:
- **Page table**: Maps virtual page numbers to local frames (8 bytes/entry)
- **Frame table**: Maps frames to the virtual page they hold
- **Free list**: Stack of available frames

Eviction uses the **clock algorithm**: a hand sweeps frames, clearing reference bits. Pages without a reference bit set are evicted. Dirty remote pages are written back before eviction.

### Page Table Entry (8 bytes, packed)

| Field        | Size | Description                           |
|--------------|------|---------------------------------------|
| frame_number | 2B   | Local frame index                     |
| valid        | 1B   | Page present in local cache           |
| reference_bit| 1B   | Accessed since last clock sweep       |
| dirty        | 1B   | Modified since fetch                  |
| location     | 1B   | INVALID / LOCAL / REMOTE              |
| pad          | 2B   | Alignment                             |

## Building

```sh
make all           # server + client
make lib           # shared library (libdsm.so)
make test          # single-node test
make cluster-test  # 2-node cluster test
```

Requires GCC with C11 support.

## Usage

### Single Server

```sh
./dsm-server -p 9999 -n 4096    # 4096 virtual pages
./dsm-client -H localhost -p 9999 -i 10000
```

### Cluster (3 nodes)

```sh
# Node 1: owns pages 0-1023
./dsm-server -p 9001 -c 10001 -r 0:1024 -n 3072

# Node 2: owns pages 1024-2047, joins via node 1
./dsm-server -p 9002 -c 10002 -r 1024:2048 -n 3072 -s localhost -S 10001

# Node 3: owns pages 2048-3071
./dsm-server -p 9003 -c 10003 -r 2048:3072 -n 3072 -s localhost -S 10001
```

### C API

```c
#include "dsm_array.h"

int fd = connect_to_server("localhost", 9999);
dsm_context_t *ctx = dsm_create_context(256, 1024);
ctx->sock_fd = fd;
dsm_init_paging_system(ctx);

size_t shape[] = {1000, 1000};
dsm_array_t *A = dsm_array_create(ctx, DSM_DOUBLE, 2, shape);

dsm_array_set_f64(A, 3.14, 50, 100);
double val = dsm_array_get_f64(A, 50, 100);

dsm_array_free(A);
dsm_destroy_context(ctx);
```

### Python API

```python
import dsm

ctx = dsm.Context("localhost", 9999, num_pages=256, num_virtual_pages=1024)
arr = dsm.Array(ctx, shape=(1000, 1000), dtype=dsm.float64)

arr[50, 100] = 3.14
print(arr[50, 100])
print(ctx.stats())
```

## Performance Characteristics

The dominant cost is network round-trip latency for page faults. With a local cache hit rate above 90%, DSM overhead approaches 1x compared to raw memory for sequential access patterns.

**Measured on localhost (3.5 GHz, TCP loopback):**

| Access Pattern       | Cache Size | Overhead vs Raw Memory |
|---------------------|------------|------------------------|
| Sequential read     | Large      | ~1.2x                  |
| Sequential read     | Small      | ~4x (eviction cost)    |
| Random read         | Large      | ~8x                    |
| Random read         | Small      | ~25x                   |

The prefetcher issues batched requests for sequential patterns, amortizing round-trip latency across multiple pages. Batch size is configurable via `ctx->prefetch_count`.

### Tuning

- **Local cache size** (`num_pages`): Larger caches reduce evictions. Size to fit working set.
- **Prefetch count**: Sequential workloads benefit from 4-8 pages. Random access should disable prefetch.
- **TCP_NODELAY**: Always enabled. Reduces latency at the cost of bandwidth efficiency.
- **Socket buffers**: Larger buffers (via `SO_RCVBUF`/`SO_SNDBUF`) improve throughput for batch operations.

## Consistency Model

This implementation provides **no coherence protocol**. Each client operates on its local cache independently. If multiple clients write to the same page:

- Last writer wins (based on writeback order)
- No read-your-writes guarantee across clients
- No atomicity for multi-page operations

For applications requiring stronger consistency, implement locking at the application layer or use this system only for read-mostly workloads with partitioned writes.

## License

MIT
