#ifndef DSM_PAGING_H
#define DSM_PAGING_H

#include <stdint.h>
#include <stddef.h>

#ifndef NUM_PAGES
#define NUM_PAGES 256
#endif

#ifndef NUM_VIRTUAL_PAGES
#define NUM_VIRTUAL_PAGES 1024
#endif

#define IS_POWER_OF_2(x) (((x) != 0) && (((x) & ((x) - 1)) == 0))

_Static_assert(IS_POWER_OF_2(NUM_PAGES), "NUM_PAGES must be a power of 2");
_Static_assert(IS_POWER_OF_2(NUM_VIRTUAL_PAGES), "NUM_VIRTUAL_PAGES must be a power of 2");

typedef enum {
    LOC_INVALID = 0,
    LOC_LOCAL,
    LOC_REMOTE
} page_location_t;

typedef struct __attribute__((packed, aligned(4))) {
    uint16_t frame_number;
    uint8_t valid;
    uint8_t reference_bit;
    uint8_t dirty;
    uint8_t location;
    uint16_t pad;
} page_table_entry_t;

typedef struct __attribute__((packed, aligned(4))) {
    uint16_t page_number;
    uint8_t occupied;
    uint8_t pad;
} frame_entry_t;

typedef struct {
    int sock_fd;
    uint8_t *local_memory;
    page_table_entry_t *page_table;
    frame_entry_t *frame_table;
    uint16_t *free_list;
    uint16_t free_list_top;
    uint16_t clock_hand;
    uint16_t num_pages;
    uint16_t num_virtual_pages;
    uint16_t free_frame_count;
    uint16_t next_alloc_page;
    uint32_t last_page_id;
    uint8_t prefetch_count;
    uint64_t local_hits;
    uint64_t remote_fetches;
    uint64_t evictions;
} dsm_context_t;

void dsm_init_paging_system(dsm_context_t *ctx);
void *dsm_access_page(dsm_context_t *ctx, uint16_t page_id, int write);
void *dsm_access_page_slow(dsm_context_t *ctx, uint16_t page_id, int write,
                           page_table_entry_t *entry);
void dsm_prefetch_pages(dsm_context_t *ctx, uint16_t start_page, uint8_t count);
dsm_context_t *dsm_create_context(uint16_t num_pages, uint16_t num_virtual_pages);
void dsm_destroy_context(dsm_context_t *ctx);

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

/* inline fast path for hot loops */
static inline void *dsm_access_page_fast(dsm_context_t *ctx, uint16_t page_id, int write)
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

