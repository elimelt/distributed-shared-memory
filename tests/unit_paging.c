/*
 * Unit tests for the pager (src/dsm_paging.c).
 *
 * Plain C, assert.h only, no framework. The process exits non-zero if
 * any assert fails.
 *
 * These tests pin CURRENT behavior in in-memory mode (ctx->sock_fd = -1,
 * see src/dsm_paging.c: dsm_access_page_slow skips the RPC fetch when
 * sock_fd < 0). In this mode there is no backing store: a page evicted
 * from the frame cache loses its content.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsm_paging.h"

#define TEST_FRAMES  8
#define TEST_VPAGES  64

static dsm_context_t *make_ctx(void)
{
    dsm_context_t *ctx = dsm_create_context(TEST_FRAMES, TEST_VPAGES);
    assert(ctx != NULL);
    /* Explicit: in-memory mode, no RPC (dsm_create_context also sets this). */
    ctx->sock_fd = -1;
    return ctx;
}

/* Test 1: second access to the same page is a hit.
 * The first access takes the slow path and does NOT count as a local hit. */
static void test_repeat_access_is_hit(void)
{
    dsm_context_t *ctx = make_ctx();

    void *first = dsm_access_page(ctx, 3, 0);
    assert(first != NULL);
    assert(ctx->local_hits == 0);

    void *second = dsm_access_page(ctx, 3, 0);
    assert(second == first);
    assert(ctx->local_hits == 1);

    /* In-memory mode never fetches remotely. */
    assert(ctx->remote_fetches == 0);

    dsm_destroy_context(ctx);
    printf("PASS test_repeat_access_is_hit\n");
}

/* Test 2: touching 9 distinct pages with an 8-frame cache evicts. */
static void test_eviction_when_cache_full(void)
{
    dsm_context_t *ctx = make_ctx();

    for (uint16_t page = 0; page < TEST_FRAMES + 1; page++) {
        void *p = dsm_access_page(ctx, page, 0);
        assert(p != NULL);
    }
    assert(ctx->evictions >= 1);

    dsm_destroy_context(ctx);
    printf("PASS test_eviction_when_cache_full\n");
}

/* Test 3: every returned pointer lies inside the mmap'd frame area and is
 * page-aligned, including after the cache is full and evictions happen. */
static void test_pointers_in_frame_area(void)
{
    dsm_context_t *ctx = make_ctx();
    uint8_t *base = ctx->local_memory;
    uint8_t *end = base + (size_t)TEST_FRAMES * PAGE_SIZE;

    for (uint16_t page = 0; page < 2 * TEST_FRAMES; page++) {
        uint8_t *p = dsm_access_page(ctx, page, 0);
        assert(p != NULL);
        assert(p >= base);
        assert(p < end);
        assert(((size_t)(p - base)) % PAGE_SIZE == 0);
        assert(((uintptr_t)p) % PAGE_SIZE == 0);
    }

    dsm_destroy_context(ctx);
    printf("PASS test_pointers_in_frame_area\n");
}

/* Test 4: with sock_fd = -1 there is no backing store. Write to a page,
 * force it out of the cache by touching 8 other pages, re-access it:
 * the content is NOT preserved. Pins current in-memory semantics. */
static void test_evicted_page_content_lost(void)
{
    dsm_context_t *ctx = make_ctx();

    uint8_t *p0 = dsm_access_page(ctx, 0, 1);
    assert(p0 != NULL);
    memset(p0, 0xAB, PAGE_SIZE);

    /* Touch 8 other pages; each gets a distinct fill (its page id). */
    for (uint16_t page = 1; page <= TEST_FRAMES; page++) {
        uint8_t *p = dsm_access_page(ctx, page, 1);
        assert(p != NULL);
        memset(p, (int)page, PAGE_SIZE);
    }

    /* Page 0 was evicted. */
    assert(ctx->evictions >= 1);
    assert(ctx->page_table[0].valid == 0);

    /* Re-access page 0: no fetch happens, old content is gone. */
    uint8_t *again = dsm_access_page(ctx, 0, 0);
    assert(again != NULL);

    uint8_t magic[PAGE_SIZE];
    memset(magic, 0xAB, PAGE_SIZE);
    assert(memcmp(again, magic, PAGE_SIZE) != 0);

    dsm_destroy_context(ctx);
    printf("PASS test_evicted_page_content_lost\n");
}

/* Test 5: destroying a fully populated context does not crash. */
static void test_destroy_populated_context(void)
{
    dsm_context_t *ctx = make_ctx();

    for (uint16_t page = 0; page < TEST_FRAMES; page++) {
        uint8_t *p = dsm_access_page(ctx, page, 1);
        assert(p != NULL);
        memset(p, (int)(page + 1), PAGE_SIZE);
    }
    assert(ctx->free_frame_count == 0);

    dsm_destroy_context(ctx);
    printf("PASS test_destroy_populated_context\n");
}

int main(void)
{
    test_repeat_access_is_hit();
    test_eviction_when_cache_full();
    test_pointers_in_frame_area();
    test_evicted_page_content_lost();
    test_destroy_populated_context();
    printf("All pager unit tests passed\n");
    return 0;
}
