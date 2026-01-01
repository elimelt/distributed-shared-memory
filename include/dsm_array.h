#ifndef DSM_ARRAY_H
#define DSM_ARRAY_H

#include "dsm_region.h"
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef enum {
    DSM_INT32 = 0,
    DSM_INT64 = 1,
    DSM_FLOAT = 2,
    DSM_DOUBLE = 3,
    DSM_UINT8 = 4
} dsm_dtype_t;

typedef struct {
    dsm_region_t *region;
    dsm_dtype_t dtype;
    size_t elem_size;
    uint32_t ndim;
    size_t *shape;
    size_t *strides;
    size_t total_elems;
} dsm_array_t;

dsm_array_t *dsm_array_create(dsm_context_t *ctx, dsm_dtype_t dtype,
                               uint32_t ndim, const size_t *shape);
void *dsm_array_ref(dsm_array_t *arr, const size_t *indices, int write);
double dsm_array_get_f64(dsm_array_t *arr, ...);
void dsm_array_set_f64(dsm_array_t *arr, double val, ...);
int32_t dsm_array_get_i32(dsm_array_t *arr, ...);
void dsm_array_set_i32(dsm_array_t *arr, int32_t val, ...);
void dsm_array_free(dsm_array_t *arr);
size_t dsm_dtype_size(dsm_dtype_t dtype);

#endif

