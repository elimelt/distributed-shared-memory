#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include "dsm_paging.h"
#include "dsm_protocol.h"

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static void *frame_to_ptr(dsm_context_t *ctx, uint32_t frame)
{
    return ctx->local_memory + ((size_t)frame * PAGE_SIZE);
}

static uint32_t get_free_frame(dsm_context_t *ctx)
{
    if (ctx->free_list_top > 0) {
        return ctx->free_list[--ctx->free_list_top];
    }
    return DSM_NO_FRAME;
}

static void push_free_frame(dsm_context_t *ctx, uint32_t frame)
{
    ctx->free_list[ctx->free_list_top++] = frame;
}

/* clock algorithm for page replacement; returns DSM_NO_FRAME if writeback fails */
static uint32_t evict_page(dsm_context_t *ctx)
{
    for (;;) {
        frame_entry_t *frame = &ctx->frame_table[ctx->clock_hand];

        if (frame->occupied) {
            page_table_entry_t *page = &ctx->page_table[frame->page_number];

            if (page->reference_bit) {
                page->reference_bit = 0;
            } else {
                uint32_t victim = ctx->clock_hand;

                if (page->dirty && page->location == LOC_REMOTE) {
                    void *data = frame_to_ptr(ctx, victim);
                    if (dsm_rpc_put(ctx->sock_fd, frame->page_number, data) < 0) {
                        fprintf(stderr, "writeback failed for page %u\n",
                                frame->page_number);
                        return DSM_NO_FRAME;
                    }
                }

                page->valid = 0;
                page->dirty = 0;
                frame->occupied = 0;
                ctx->clock_hand = (ctx->clock_hand + 1) % ctx->num_pages;
                ctx->evictions++;

                push_free_frame(ctx, victim);
                return get_free_frame(ctx);
            }
        }

        ctx->clock_hand = (ctx->clock_hand + 1) % ctx->num_pages;
    }
}

void dsm_init_paging_system(dsm_context_t *ctx)
{
    memset(ctx->page_table, 0, ctx->num_virtual_pages * sizeof(page_table_entry_t));
    memset(ctx->frame_table, 0, ctx->num_pages * sizeof(frame_entry_t));

    for (uint32_t i = 0; i < ctx->num_pages; i++)
        ctx->free_list[i] = ctx->num_pages - 1 - i;
    ctx->free_list_top = ctx->num_pages;

    ctx->clock_hand = 0;
    ctx->last_page_id = DSM_NO_PAGE;
    ctx->prefetch_count = 4;
    ctx->local_hits = 0;
    ctx->remote_fetches = 0;
    ctx->evictions = 0;
}

void dsm_prefetch_pages(dsm_context_t *ctx, uint32_t start_page, uint8_t count)
{
    uint32_t page_ids[DSM_PREFETCH_MAX];
    uint32_t frames[DSM_PREFETCH_MAX];
    uint8_t bufs[DSM_PREFETCH_MAX * PAGE_SIZE];
    int batch_count = 0;

    for (uint8_t i = 0; i < count && batch_count < DSM_PREFETCH_MAX; i++) {
        uint32_t page_id = start_page + i;
        if (page_id >= ctx->num_virtual_pages)
            break;

        page_table_entry_t *entry = &ctx->page_table[page_id];
        if (entry->valid || entry->location != LOC_REMOTE)
            continue;

        uint32_t frame = get_free_frame(ctx);
        if (frame == DSM_NO_FRAME)
            break;

        page_ids[batch_count] = page_id;
        frames[batch_count] = frame;
        batch_count++;
    }

    if (batch_count == 0)
        return;

    if (dsm_rpc_get_batch(ctx->sock_fd, page_ids, batch_count, bufs) < 0) {
        for (int i = 0; i < batch_count; i++)
            push_free_frame(ctx, frames[i]);
        return;
    }

    for (int i = 0; i < batch_count; i++) {
        uint32_t page_id = page_ids[i];
        page_table_entry_t *entry = &ctx->page_table[page_id];

        memcpy(frame_to_ptr(ctx, frames[i]), bufs + (size_t)i * PAGE_SIZE, PAGE_SIZE);

        entry->frame_number = frames[i];
        entry->valid = 1;
        entry->reference_bit = 0;
        entry->dirty = 0;

        ctx->frame_table[frames[i]].page_number = page_id;
        ctx->frame_table[frames[i]].occupied = 1;
        ctx->remote_fetches++;
    }
}

void *dsm_access_page(dsm_context_t *ctx, uint32_t page_id, int write)
{
    page_table_entry_t *entry = &ctx->page_table[page_id];

    __builtin_prefetch(&ctx->page_table[page_id + 1], 0, 3);

    if (likely(entry->valid)) {
        ctx->local_hits++;
        entry->reference_bit = 1;
        entry->dirty |= (uint8_t)write;
        return frame_to_ptr(ctx, entry->frame_number);
    }

    return dsm_access_page_slow(ctx, page_id, write, entry);
}

__attribute__((noinline, cold))
void *dsm_access_page_slow(dsm_context_t *ctx, uint32_t page_id, int write,
                           page_table_entry_t *entry)
{
    uint32_t frame = get_free_frame(ctx);
    if (unlikely(frame == DSM_NO_FRAME)) {
        frame = evict_page(ctx);
        if (unlikely(frame == DSM_NO_FRAME))
            return NULL;
    }

    void *frame_ptr = frame_to_ptr(ctx, frame);

    int should_fetch = (ctx->sock_fd >= 0);
    if (should_fetch) {
        if (unlikely(dsm_rpc_get(ctx->sock_fd, page_id, frame_ptr) < 0)) {
            fprintf(stderr, "RPC fetch failed for page %u\n", page_id);
            push_free_frame(ctx, frame);
            return NULL;
        }
        ctx->remote_fetches++;
    }

    entry->frame_number = frame;
    entry->valid = 1;
    entry->reference_bit = 1;
    entry->dirty = (uint8_t)write;
    entry->location = should_fetch ? LOC_REMOTE : LOC_LOCAL;

    ctx->frame_table[frame].page_number = page_id;
    ctx->frame_table[frame].occupied = 1;

    int is_sequential = (page_id == ctx->last_page_id + 1);
    if (is_sequential)
        dsm_prefetch_pages(ctx, page_id + 1, ctx->prefetch_count);
    ctx->last_page_id = page_id;

    return frame_ptr;
}

dsm_context_t *dsm_create_context(uint32_t num_pages, uint32_t num_virtual_pages)
{
    dsm_context_t *ctx = malloc(sizeof(dsm_context_t));
    if (!ctx)
        return NULL;

    ctx->local_memory = mmap(NULL, (size_t)num_pages * PAGE_SIZE,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ctx->local_memory == MAP_FAILED) {
        free(ctx);
        return NULL;
    }

    ctx->page_table = calloc(num_virtual_pages, sizeof(page_table_entry_t));
    ctx->frame_table = calloc(num_pages, sizeof(frame_entry_t));
    ctx->free_list = malloc(num_pages * sizeof(uint32_t));

    if (!ctx->page_table || !ctx->frame_table || !ctx->free_list) {
        munmap(ctx->local_memory, (size_t)num_pages * PAGE_SIZE);
        free(ctx->page_table);
        free(ctx->frame_table);
        free(ctx->free_list);
        free(ctx);
        return NULL;
    }

    for (uint32_t i = 0; i < num_pages; i++)
        ctx->free_list[i] = num_pages - 1 - i;
    ctx->free_list_top = num_pages;

    ctx->num_pages = num_pages;
    ctx->num_virtual_pages = num_virtual_pages;
    ctx->sock_fd = -1;
    ctx->clock_hand = 0;
    ctx->next_alloc_page = 0;
    ctx->last_page_id = DSM_NO_PAGE;
    ctx->prefetch_count = 4;
    ctx->local_hits = 0;
    ctx->remote_fetches = 0;
    ctx->evictions = 0;

    return ctx;
}

void dsm_destroy_context(dsm_context_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->local_memory && ctx->local_memory != MAP_FAILED)
        munmap(ctx->local_memory, (size_t)ctx->num_pages * PAGE_SIZE);

    free(ctx->page_table);
    free(ctx->frame_table);
    free(ctx->free_list);
    free(ctx);
}

