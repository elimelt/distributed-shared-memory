#ifndef DSM_PAGING_H
#define DSM_PAGING_H

#include <stdint.h>
#include <stddef.h>

#define DSM_NO_FRAME UINT32_MAX
#define DSM_NO_PAGE UINT32_MAX
#define DSM_PREFETCH_MAX 8

typedef enum {
    LOC_LOCAL,
    LOC_REMOTE
} page_location_t;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t frame_number;
    uint8_t valid;
    uint8_t reference_bit;
    uint8_t dirty;
    uint8_t location;
} page_table_entry_t;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t page_number;
    uint8_t occupied;
    uint8_t pad;
} frame_entry_t;

typedef struct {
    int sock_fd;
    uint8_t *local_memory;
    page_table_entry_t *page_table;
    frame_entry_t *frame_table;
    uint32_t *free_list;
    uint32_t free_list_top;
    uint32_t clock_hand;
    uint32_t num_pages;
    uint32_t num_virtual_pages;
    uint32_t next_alloc_page;
    uint32_t last_page_id;
    uint8_t prefetch_count;
    uint64_t local_hits;
    uint64_t remote_fetches;
    uint64_t evictions;
} dsm_context_t;

void dsm_init_paging_system(dsm_context_t *ctx);
void *dsm_access_page(dsm_context_t *ctx, uint32_t page_id, int write);
void *dsm_access_page_slow(dsm_context_t *ctx, uint32_t page_id, int write,
                           page_table_entry_t *entry);
void dsm_prefetch_pages(dsm_context_t *ctx, uint32_t start_page, uint8_t count);
dsm_context_t *dsm_create_context(uint32_t num_pages, uint32_t num_virtual_pages);
void dsm_destroy_context(dsm_context_t *ctx);

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* inline fast path for hot loops */
static inline void *dsm_access_page_fast(dsm_context_t *ctx, uint32_t page_id, int write)
{
    page_table_entry_t *entry = &ctx->page_table[page_id];

    if (__builtin_expect(entry->valid, 1)) {
        ctx->local_hits++;
        entry->reference_bit = 1;
        entry->dirty |= (uint8_t)write;
        return ctx->local_memory + (size_t)entry->frame_number * PAGE_SIZE;
    }

    return dsm_access_page_slow(ctx, page_id, write, entry);
}

#endif /* DSM_PAGING_H */

