"""End-to-end test for the DSM Python bindings.

Requires a running dsm-server (default localhost:9999, override with
DSM_HOST/DSM_PORT). Exits nonzero on any failure.
"""

import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python'))

import dsm  # noqa: E402

HOST = os.environ.get('DSM_HOST', '127.0.0.1')
PORT = int(os.environ.get('DSM_PORT', '9999'))


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print(f"ok: {msg}")


def test_two_arrays_disjoint_and_round_trip():
    # Small local cache forces evictions, so writes round-trip
    # through the server (writeback, then refetch).
    ctx = dsm.Context(HOST, PORT, local_pages=8, virtual_pages=256)
    try:
        a = dsm.Array(ctx, shape=(1024,), dtype=dsm.int64)     # 2 pages
        b = dsm.Array(ctx, shape=(64, 64), dtype=dsm.float64)  # 8 pages

        a_range = range(a._base_page, a._base_page + a._num_pages)
        b_range = range(b._base_page, b._base_page + b._num_pages)
        check(set(a_range).isdisjoint(set(b_range)),
              f"array page ranges disjoint ({a_range} vs {b_range})")

        # Write through both arrays.
        for i in range(0, 1024, 64):
            a[i] = i * 3
        for i in range(64):
            b[i, i] = i + 0.5

        # Touch a third array larger than the local cache. This
        # evicts a's and b's dirty pages back to the server.
        c = dsm.Array(ctx, shape=(16 * 512,), dtype=dsm.int64)  # 16 pages
        for i in range(0, 16 * 512, 512):
            c[i] = i

        # Read back: pages must be refetched from the server.
        for i in range(0, 1024, 64):
            check(a[i] == i * 3, f"a[{i}] == {i * 3} after round trip")
        for i in range(0, 64, 16):
            check(b[i, i] == i + 0.5, f"b[{i},{i}] == {i + 0.5} after round trip")

        stats = ctx.stats
        check(stats['remote_fetches'] > 0,
              f"stats reports nonzero fetches ({stats})")
        check(stats['evictions'] > 0,
              f"stats reports nonzero evictions ({stats})")
    finally:
        ctx.close()


def test_alloc_exhaustion():
    with dsm.Context(HOST, PORT, local_pages=4, virtual_pages=8) as ctx:
        dsm.Array(ctx, shape=(4 * 512,), dtype=dsm.int64)  # 4 of 8 pages
        try:
            dsm.Array(ctx, shape=(8 * 512,), dtype=dsm.int64)  # 8 more
        except MemoryError:
            print("ok: over-allocation raises MemoryError")
        else:
            raise AssertionError("over-allocation did not raise MemoryError")


def test_context_manager():
    with dsm.Context(HOST, PORT, local_pages=4, virtual_pages=16) as ctx:
        arr = dsm.Array(ctx, shape=(10,), dtype=dsm.float64)
        arr[3] = 42.0
        check(arr[3] == 42.0, "read back inside context manager")
    check(ctx._ctx is None, "context handle released on __exit__")
    try:
        ctx.access_page(0)
    except RuntimeError:
        print("ok: access after close raises RuntimeError")
    else:
        raise AssertionError("access after close did not raise")


def main():
    tests = [
        test_two_arrays_disjoint_and_round_trip,
        test_alloc_exhaustion,
        test_context_manager,
    ]
    for test in tests:
        print(f"--- {test.__name__}")
        test()
    print("PASS: all python e2e tests passed")


if __name__ == '__main__':
    try:
        main()
    except Exception:
        traceback.print_exc()
        print("FAIL: python e2e test failed")
        sys.exit(1)
