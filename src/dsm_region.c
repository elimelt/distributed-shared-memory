#include "dsm_region.h"
#include "dsm_protocol.h"
#include <stdlib.h>
#include <stdint.h>

dsm_region_t *dsm_region_alloc(dsm_context_t *ctx, size_t size)
{
    if (!ctx || size == 0)
        return NULL;

    dsm_region_t *region = malloc(sizeof(dsm_region_t));
    if (!region)
        return NULL;

    uint32_t num_pages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    uint32_t base_page = ctx->next_alloc_page;

    if (base_page + num_pages > ctx->num_virtual_pages) {
        free(region);
        return NULL;
    }

    ctx->next_alloc_page += num_pages;

    region->ctx = ctx;
    region->base_page = base_page;
    region->num_pages = num_pages;
    region->size = size;

    return region;
}

void *dsm_region_ref(dsm_region_t *region, size_t offset, int write)
{
    if (!region || !region->ctx)
        return NULL;

    if (offset >= region->size)
        return NULL;

    uint32_t page_offset = (uint32_t)(offset / PAGE_SIZE);
    uint32_t page_id = region->base_page + page_offset;

    void *page_ptr = dsm_access_page(region->ctx, (uint16_t)page_id, write);
    if (!page_ptr)
        return NULL;

    size_t byte_offset = offset % PAGE_SIZE;
    return (uint8_t *)page_ptr + byte_offset;
}

void dsm_region_free(dsm_region_t *region)
{
    free(region);
}

