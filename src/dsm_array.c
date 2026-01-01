#include "dsm_array.h"
#include <stdlib.h>
#include <string.h>

size_t dsm_dtype_size(dsm_dtype_t dtype)
{
    switch (dtype) {
        case DSM_INT32:  return 4;
        case DSM_INT64:  return 8;
        case DSM_FLOAT:  return 4;
        case DSM_DOUBLE: return 8;
        case DSM_UINT8:  return 1;
        default:         return 0;
    }
}

dsm_array_t *dsm_array_create(dsm_context_t *ctx, dsm_dtype_t dtype,
                               uint32_t ndim, const size_t *shape)
{
    if (!ctx || ndim == 0 || !shape)
        return NULL;

    dsm_array_t *arr = malloc(sizeof(dsm_array_t));
    if (!arr)
        return NULL;

    arr->dtype = dtype;
    arr->elem_size = dsm_dtype_size(dtype);
    arr->ndim = ndim;

    arr->shape = malloc(ndim * sizeof(size_t));
    arr->strides = malloc(ndim * sizeof(size_t));
    if (!arr->shape || !arr->strides) {
        free(arr->shape);
        free(arr->strides);
        free(arr);
        return NULL;
    }

    arr->total_elems = 1;
    for (uint32_t i = 0; i < ndim; i++) {
        arr->shape[i] = shape[i];
        arr->total_elems *= shape[i];
    }

    arr->strides[ndim - 1] = arr->elem_size;
    for (int i = (int)ndim - 2; i >= 0; i--) {
        arr->strides[i] = arr->strides[i + 1] * arr->shape[i + 1];
    }

    size_t total_size = arr->total_elems * arr->elem_size;
    arr->region = dsm_region_alloc(ctx, total_size);
    if (!arr->region) {
        free(arr->shape);
        free(arr->strides);
        free(arr);
        return NULL;
    }

    return arr;
}

void *dsm_array_ref(dsm_array_t *arr, const size_t *indices, int write)
{
    if (!arr || !indices)
        return NULL;

    size_t offset = 0;
    for (uint32_t i = 0; i < arr->ndim; i++) {
#ifdef DSM_ARRAY_BOUNDS_CHECK
        if (indices[i] >= arr->shape[i])
            return NULL;
#endif
        offset += indices[i] * arr->strides[i];
    }

    return dsm_region_ref(arr->region, offset, write);
}

double dsm_array_get_f64(dsm_array_t *arr, ...)
{
    if (!arr)
        return 0.0;

    size_t indices[arr->ndim];
    va_list args;
    va_start(args, arr);
    for (uint32_t i = 0; i < arr->ndim; i++) {
        indices[i] = va_arg(args, size_t);
    }
    va_end(args);

    void *ptr = dsm_array_ref(arr, indices, 0);
    if (!ptr)
        return 0.0;

    return *(double *)ptr;
}

void dsm_array_set_f64(dsm_array_t *arr, double val, ...)
{
    if (!arr)
        return;

    size_t indices[arr->ndim];
    va_list args;
    va_start(args, val);
    for (uint32_t i = 0; i < arr->ndim; i++) {
        indices[i] = va_arg(args, size_t);
    }
    va_end(args);

    void *ptr = dsm_array_ref(arr, indices, 1);
    if (ptr)
        *(double *)ptr = val;
}

int32_t dsm_array_get_i32(dsm_array_t *arr, ...)
{
    if (!arr)
        return 0;

    size_t indices[arr->ndim];
    va_list args;
    va_start(args, arr);
    for (uint32_t i = 0; i < arr->ndim; i++) {
        indices[i] = va_arg(args, size_t);
    }
    va_end(args);

    void *ptr = dsm_array_ref(arr, indices, 0);
    if (!ptr)
        return 0;

    return *(int32_t *)ptr;
}

void dsm_array_set_i32(dsm_array_t *arr, int32_t val, ...)
{
    if (!arr)
        return;

    size_t indices[arr->ndim];
    va_list args;
    va_start(args, val);
    for (uint32_t i = 0; i < arr->ndim; i++) {
        indices[i] = va_arg(args, size_t);
    }
    va_end(args);

    void *ptr = dsm_array_ref(arr, indices, 1);
    if (ptr)
        *(int32_t *)ptr = val;
}

void dsm_array_free(dsm_array_t *arr)
{
    if (!arr)
        return;

    dsm_region_free(arr->region);
    free(arr->shape);
    free(arr->strides);
    free(arr);
}

