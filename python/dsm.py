"""
DSM (Distributed Shared Memory) Python Bindings

Usage:
    import dsm

    with dsm.Context("localhost", 9999,
                     local_pages=256, virtual_pages=4096) as ctx:
        arr = dsm.Array(ctx, shape=(1000, 1000), dtype=dsm.float64)
        arr[50, 100] = 3.14
        print(arr[50, 100])
"""

import ctypes
from ctypes import (c_void_p, c_uint32, c_int, c_double,
                    c_int32, c_float, c_int64, c_uint8, c_uint64, POINTER,
                    byref)
import os
import socket
from functools import reduce
from operator import mul

# Load the shared library (DSM_LIB overrides the repo-relative default)
_lib_path = os.environ.get(
    'DSM_LIB',
    os.path.join(os.path.dirname(__file__), '..', 'libdsm.so'))
try:
    _lib = ctypes.CDLL(_lib_path)
except OSError as e:
    raise ImportError(f"Failed to load DSM library from {_lib_path}: {e}")

# Constants from C library
PAGE_SIZE = 4096
DEFAULT_PORT = 9999
DEFAULT_HOST = "127.0.0.1"

# Data types enum
int32 = 0
int64 = 1
float32 = 2
float64 = 3
uint8 = 4

# Map dtype to ctypes type and element size
_dtype_map = {
    int32: (c_int32, 4),
    int64: (c_int64, 8),
    float32: (c_float, 4),
    float64: (c_double, 8),
    uint8: (c_uint8, 1),
}

# Define function signatures for DSM library
_lib.dsm_create_context.argtypes = [c_uint32, c_uint32]
_lib.dsm_create_context.restype = c_void_p

_lib.dsm_destroy_context.argtypes = [c_void_p]
_lib.dsm_destroy_context.restype = None

_lib.dsm_access_page.argtypes = [c_void_p, c_uint32, c_int]
_lib.dsm_access_page.restype = c_void_p

_lib.dsm_prefetch_pages.argtypes = [c_void_p, c_uint32, c_uint8]
_lib.dsm_prefetch_pages.restype = None

_lib.dsm_context_set_socket.argtypes = [c_void_p, c_int]
_lib.dsm_context_set_socket.restype = None

_lib.dsm_context_get_stats.argtypes = [c_void_p, POINTER(c_uint64),
                                       POINTER(c_uint64), POINTER(c_uint64)]
_lib.dsm_context_get_stats.restype = None


class Context:
    """DSM context - connection to the distributed memory system."""

    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT,
                 local_pages=256, virtual_pages=4096):
        """
        Create a DSM context and connect to the server.

        Args:
            host: Server hostname or IP address
            port: Server port number
            local_pages: Number of local page frames (cache size)
            virtual_pages: Total virtual address space in pages
        """
        self._ctx = None
        self._sock = None
        self._next_page = 0

        ctx = _lib.dsm_create_context(local_pages, virtual_pages)
        if not ctx:
            raise RuntimeError("Failed to create DSM context")

        self.host = host
        self.port = port
        self.local_pages = local_pages
        self.virtual_pages = virtual_pages

        # Connect to server
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.connect((host, port))
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except socket.error as e:
            sock.close()
            _lib.dsm_destroy_context(ctx)
            raise ConnectionError(f"Failed to connect to {host}:{port}: {e}")

        _lib.dsm_context_set_socket(ctx, sock.fileno())
        self._ctx = ctx
        self._sock = sock

    def _alloc_pages(self, count):
        """Reserve count contiguous virtual pages; return the first page id."""
        if self._ctx is None:
            raise RuntimeError("Context is closed")
        if self._next_page + count > self.virtual_pages:
            raise MemoryError(
                f"Cannot allocate {count} pages: "
                f"{self.virtual_pages - self._next_page} of "
                f"{self.virtual_pages} virtual pages remain")
        base = self._next_page
        self._next_page += count
        return base

    def access_page(self, page_id, write=False):
        """Access a page, returning a pointer to its data."""
        if self._ctx is None:
            raise RuntimeError("Context is closed")
        ptr = _lib.dsm_access_page(self._ctx, page_id, int(write))
        if not ptr:
            raise RuntimeError(f"Failed to access page {page_id}")
        return ptr

    def prefetch(self, start_page, count=4):
        """Prefetch pages starting from start_page."""
        if self._ctx is None:
            raise RuntimeError("Context is closed")
        _lib.dsm_prefetch_pages(self._ctx, start_page, count)

    @property
    def stats(self):
        """Return context statistics (local_hits, remote_fetches, evictions)."""
        if self._ctx is None:
            raise RuntimeError("Context is closed")
        hits = c_uint64()
        fetches = c_uint64()
        evictions = c_uint64()
        _lib.dsm_context_get_stats(self._ctx, byref(hits), byref(fetches),
                                   byref(evictions))
        return {
            'local_hits': hits.value,
            'remote_fetches': fetches.value,
            'evictions': evictions.value,
        }

    def close(self):
        """Release the context and close the server connection."""
        if self._ctx is not None:
            _lib.dsm_destroy_context(self._ctx)
            self._ctx = None
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()
        return False

    def __del__(self):
        self.close()


class Array:
    """N-dimensional distributed array stored in DSM pages."""

    def __init__(self, ctx, shape, dtype=float64):
        """
        Create a distributed array.

        Args:
            ctx: DSM Context object
            shape: Tuple of dimensions (e.g., (1000, 1000))
            dtype: Data type (int32, int64, float32, float64, uint8)
        """
        self.ctx = ctx
        self.shape = tuple(shape)
        self.dtype = dtype
        self.ndim = len(shape)
        self._ctype, self._elem_size = _dtype_map[dtype]
        self.size = reduce(mul, shape, 1)
        self._strides = self._compute_strides()
        self._elems_per_page = PAGE_SIZE // self._elem_size
        num_pages = -(-(self.size * self._elem_size) // PAGE_SIZE)
        self._num_pages = num_pages
        self._base_page = ctx._alloc_pages(num_pages)

    def _compute_strides(self):
        """Compute row-major strides for the array."""
        strides = [1] * self.ndim
        for i in range(self.ndim - 2, -1, -1):
            strides[i] = strides[i + 1] * self.shape[i + 1]
        return tuple(strides)

    def _flat_index(self, indices):
        """Convert N-d indices to flat index."""
        if len(indices) != self.ndim:
            raise IndexError(f"Expected {self.ndim} indices, got {len(indices)}")
        flat = 0
        for i, (idx, stride, dim) in enumerate(zip(indices, self._strides, self.shape)):
            if idx < 0:
                idx += dim
            if not (0 <= idx < dim):
                raise IndexError(f"Index {idx} out of bounds for axis {i} with size {dim}")
            flat += idx * stride
        return flat

    def __getitem__(self, indices):
        """Get element at the given indices."""
        if not isinstance(indices, tuple):
            indices = (indices,)
        flat_idx = self._flat_index(indices)
        page_id = self._base_page + flat_idx // self._elems_per_page
        offset = (flat_idx % self._elems_per_page) * self._elem_size
        ptr = self.ctx.access_page(page_id, write=False)
        return ctypes.cast(ptr + offset, POINTER(self._ctype))[0]

    def __setitem__(self, indices, value):
        """Set element at the given indices."""
        if not isinstance(indices, tuple):
            indices = (indices,)
        flat_idx = self._flat_index(indices)
        page_id = self._base_page + flat_idx // self._elems_per_page
        offset = (flat_idx % self._elems_per_page) * self._elem_size
        ptr = self.ctx.access_page(page_id, write=True)
        ctypes.cast(ptr + offset, POINTER(self._ctype))[0] = value

    def __len__(self):
        return self.shape[0] if self.shape else 0

    def __repr__(self):
        return f"dsm.Array(shape={self.shape}, dtype={self.dtype})"


def to_numpy(arr):
    """
    Copy DSM array to a local NumPy array.

    Args:
        arr: DSM Array object

    Returns:
        numpy.ndarray with the same shape and dtype
    """
    try:
        import numpy as np
    except ImportError:
        raise ImportError("NumPy is required for to_numpy()")

    dtype_map = {
        int32: np.int32,
        int64: np.int64,
        float32: np.float32,
        float64: np.float64,
        uint8: np.uint8,
    }
    result = np.empty(arr.shape, dtype=dtype_map[arr.dtype])

    # Iterate through flat indices and copy
    for flat_idx in range(arr.size):
        # Convert flat index back to N-d indices
        indices = []
        remaining = flat_idx
        for stride in arr._strides:
            indices.append(remaining // stride)
            remaining %= stride
        result[tuple(indices)] = arr[tuple(indices)]

    return result


def from_numpy(ctx, np_arr):
    """
    Create a DSM array from a NumPy array.

    Args:
        ctx: DSM Context object
        np_arr: NumPy array to copy from

    Returns:
        DSM Array with the same data
    """
    try:
        import numpy as np
    except ImportError:
        raise ImportError("NumPy is required for from_numpy()")

    dtype_map = {
        np.dtype('int32'): int32,
        np.dtype('int64'): int64,
        np.dtype('float32'): float32,
        np.dtype('float64'): float64,
        np.dtype('uint8'): uint8,
    }

    dsm_dtype = dtype_map.get(np_arr.dtype)
    if dsm_dtype is None:
        raise TypeError(f"Unsupported dtype: {np_arr.dtype}")

    result = Array(ctx, np_arr.shape, dtype=dsm_dtype)

    # Copy elements
    it = np.nditer(np_arr, flags=['multi_index'])
    while not it.finished:
        result[it.multi_index] = it[0].item()
        it.iternext()

    return result

