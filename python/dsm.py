"""
DSM (Distributed Shared Memory) Python Bindings

Usage:
    import dsm

    ctx = dsm.Context("localhost", 9999)
    arr = dsm.Array(ctx, shape=(1000, 1000), dtype=dsm.float64)
    arr[50, 100] = 3.14
    print(arr[50, 100])
"""

import ctypes
from ctypes import (c_void_p, c_uint16, c_uint32, c_size_t, c_int, c_double,
                    c_int32, c_float, c_int64, c_uint8, c_uint64, POINTER)
import os
import socket
import struct
from functools import reduce
from operator import mul

# Load the shared library
_lib_path = os.path.join(os.path.dirname(__file__), '..', 'libdsm.so')
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
    int32: (c_int32, 4, 'i'),
    int64: (c_int64, 8, 'q'),
    float32: (c_float, 4, 'f'),
    float64: (c_double, 8, 'd'),
    uint8: (c_uint8, 1, 'B'),
}

# Define function signatures for DSM library
_lib.dsm_create_context.argtypes = [c_uint16, c_uint16]
_lib.dsm_create_context.restype = c_void_p

_lib.dsm_destroy_context.argtypes = [c_void_p]
_lib.dsm_destroy_context.restype = None

_lib.dsm_access_page.argtypes = [c_void_p, c_uint16, c_int]
_lib.dsm_access_page.restype = c_void_p

_lib.dsm_init_paging_system.argtypes = [c_void_p]
_lib.dsm_init_paging_system.restype = None

_lib.dsm_prefetch_pages.argtypes = [c_void_p, c_uint16, c_uint8]
_lib.dsm_prefetch_pages.restype = None


# Define the context structure for accessing stats
class _DsmContext(ctypes.Structure):
    """Mirror of dsm_context_t C struct for field access."""
    _fields_ = [
        ("sock_fd", c_int),
        ("local_memory", c_void_p),
        ("page_table", c_void_p),
        ("frame_table", c_void_p),
        ("free_list", c_void_p),
        ("free_list_top", c_uint16),
        ("clock_hand", c_uint16),
        ("num_pages", c_uint16),
        ("num_virtual_pages", c_uint16),
        ("free_frame_count", c_uint16),
        ("next_alloc_page", c_uint16),
        ("last_page_id", c_uint32),
        ("prefetch_count", c_uint8),
        ("_pad2", c_uint8 * 7),
        ("local_hits", c_uint64),
        ("remote_fetches", c_uint64),
        ("evictions", c_uint64),
    ]


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
        self._ctx = _lib.dsm_create_context(local_pages, virtual_pages)
        if not self._ctx:
            raise RuntimeError("Failed to create DSM context")

        self.host = host
        self.port = port
        self.local_pages = local_pages
        self.virtual_pages = virtual_pages

        # Connect to server
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            self._sock.connect((host, port))
            self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            # Store socket fd in context struct (offset of sock_fd is 0)
            sock_fd = self._sock.fileno()
            ctypes.cast(self._ctx, POINTER(c_int))[0] = sock_fd
        except socket.error as e:
            _lib.dsm_destroy_context(self._ctx)
            raise ConnectionError(f"Failed to connect to {host}:{port}: {e}")

    def access_page(self, page_id, write=False):
        """Access a page, returning a pointer to its data."""
        ptr = _lib.dsm_access_page(self._ctx, page_id, int(write))
        if not ptr:
            raise RuntimeError(f"Failed to access page {page_id}")
        return ptr

    def prefetch(self, start_page, count=4):
        """Prefetch pages starting from start_page."""
        _lib.dsm_prefetch_pages(self._ctx, start_page, count)

    @property
    def stats(self):
        """Return context statistics (local_hits, remote_fetches, evictions)."""
        ctx_struct = ctypes.cast(self._ctx, POINTER(_DsmContext)).contents
        return {
            'local_hits': ctx_struct.local_hits,
            'remote_fetches': ctx_struct.remote_fetches,
            'evictions': ctx_struct.evictions,
        }

    def __del__(self):
        if hasattr(self, '_sock') and self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
        if hasattr(self, '_ctx') and self._ctx:
            _lib.dsm_destroy_context(self._ctx)


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
        self._ctype, self._elem_size, self._struct_fmt = _dtype_map[dtype]
        self.size = reduce(mul, shape, 1)
        self._strides = self._compute_strides()
        self._elems_per_page = PAGE_SIZE // self._elem_size

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
        page_id = flat_idx // self._elems_per_page
        offset = (flat_idx % self._elems_per_page) * self._elem_size
        ptr = self.ctx.access_page(page_id, write=False)
        return ctypes.cast(ptr + offset, POINTER(self._ctype))[0]

    def __setitem__(self, indices, value):
        """Set element at the given indices."""
        if not isinstance(indices, tuple):
            indices = (indices,)
        flat_idx = self._flat_index(indices)
        page_id = flat_idx // self._elems_per_page
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

