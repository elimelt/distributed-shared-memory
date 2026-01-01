#ifndef DSM_REGION_H
#define DSM_REGION_H

#include "dsm_paging.h"
#include <stddef.h>

typedef struct {
    dsm_context_t *ctx;
    uint32_t base_page;
    uint32_t num_pages;
    size_t size;
} dsm_region_t;

dsm_region_t *dsm_region_alloc(dsm_context_t *ctx, size_t size);
void *dsm_region_ref(dsm_region_t *region, size_t offset, int write);
void dsm_region_free(dsm_region_t *region);

#endif

