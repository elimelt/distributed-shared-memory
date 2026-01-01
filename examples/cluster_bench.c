// benchmark for matrix multiply with pages distributed across multiple servers

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "dsm_array.h"
#include "dsm_protocol.h"

#define PAGE_SIZE 4096

static inline uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static int connect_to_server(const char *host, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static volatile double sink = 0.0;

static uint64_t bench_distributed_matmul(dsm_context_t *ctx, size_t N, int iterations)
{
    size_t bytes_per_matrix = N * N * sizeof(double);
    size_t pages_per_matrix = (bytes_per_matrix + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t elems_per_page = PAGE_SIZE / sizeof(double);

    // A on server 1, B on server 2, C split across both
    uint16_t base_A = 0;
    uint16_t base_B = 4096;
    uint16_t base_C = 4096 - pages_per_matrix/2;

    ctx->next_alloc_page = base_C + pages_per_matrix;

    printf("  Matrix layout (distributed):\n");
    printf("    A[pages %u-%u] -> Server 1\n", base_A, base_A + (uint16_t)pages_per_matrix - 1);
    printf("    B[pages %u-%u] -> Server 2\n", base_B, base_B + (uint16_t)pages_per_matrix - 1);
    printf("    C[pages %u-%u] -> Split across both\n", base_C, base_C + (uint16_t)pages_per_matrix - 1);

    double *page_ptr = NULL;
    uint16_t cur_page = 0xFFFF;

    printf("  Initializing matrices...\n");
    for (size_t i = 0; i < N * N; i++) {
        uint16_t pid = base_A + (i / elems_per_page);
        if (pid != cur_page) {
            page_ptr = dsm_access_page_fast(ctx, pid, 1);
            cur_page = pid;
        }
        page_ptr[i % elems_per_page] = (double)(i % 100) / 100.0;
    }

    cur_page = 0xFFFF;
    for (size_t i = 0; i < N * N; i++) {
        uint16_t pid = base_B + (i / elems_per_page);
        if (pid != cur_page) {
            page_ptr = dsm_access_page_fast(ctx, pid, 1);
            cur_page = pid;
        }
        page_ptr[i % elems_per_page] = (double)((i * 7) % 100) / 100.0;
    }

    ctx->local_hits = 0;
    ctx->remote_fetches = 0;

    printf("  Running matrix multiplication (%d iterations)...\n", iterations);
    uint64_t total_us = 0;

    for (int iter = 0; iter < iterations; iter++) {
        for (size_t p = 0; p < pages_per_matrix; p++) {
            memset(dsm_access_page_fast(ctx, base_C + p, 1), 0, PAGE_SIZE);
        }

        uint64_t start = now_us();

        double *page_a = NULL, *page_b = NULL, *page_c = NULL;
        uint16_t pid_a = 0xFFFF, pid_b = 0xFFFF, pid_c = 0xFFFF;

        for (size_t i = 0; i < N; i++) {
            for (size_t k = 0; k < N; k++) {
                size_t idx_a = i * N + k;
                uint16_t new_pid_a = base_A + (idx_a / elems_per_page);
                if (new_pid_a != pid_a) {
                    page_a = dsm_access_page_fast(ctx, new_pid_a, 0);
                    pid_a = new_pid_a;
                }
                double a_ik = page_a[idx_a % elems_per_page];

                for (size_t j = 0; j < N; j++) {
                    size_t idx_b = k * N + j;
                    uint16_t new_pid_b = base_B + (idx_b / elems_per_page);
                    if (new_pid_b != pid_b) {
                        page_b = dsm_access_page_fast(ctx, new_pid_b, 0);
                        pid_b = new_pid_b;
                    }

                    size_t idx_c = i * N + j;
                    uint16_t new_pid_c = base_C + (idx_c / elems_per_page);
                    if (new_pid_c != pid_c) {
                        page_c = dsm_access_page_fast(ctx, new_pid_c, 1);
                        pid_c = new_pid_c;
                    }

                    page_c[idx_c % elems_per_page] += a_ik * page_b[idx_b % elems_per_page];
                }
            }
        }

        uint64_t end = now_us();
        total_us += (end - start);

        size_t mid = (N/2) * N + (N/2);
        sink = *((double *)dsm_access_page_fast(ctx, base_C + mid / elems_per_page, 0)
                 + (mid % elems_per_page));
    }

    return total_us / iterations;
}

static uint64_t bench_raw_matmul(size_t N, int iterations)
{
    double *A = aligned_alloc(64, N * N * sizeof(double));
    double *B = aligned_alloc(64, N * N * sizeof(double));
    double *C = aligned_alloc(64, N * N * sizeof(double));

    for (size_t i = 0; i < N * N; i++) {
        A[i] = (double)(i % 100) / 100.0;
        B[i] = (double)((i * 7) % 100) / 100.0;
    }

    uint64_t total_us = 0;

    for (int iter = 0; iter < iterations; iter++) {
        memset(C, 0, N * N * sizeof(double));

        uint64_t start = now_us();

        for (size_t i = 0; i < N; i++) {
            for (size_t k = 0; k < N; k++) {
                double a_ik = A[i * N + k];
                for (size_t j = 0; j < N; j++) {
                    C[i * N + j] += a_ik * B[k * N + j];
                }
            }
        }

        uint64_t end = now_us();
        total_us += (end - start);
        sink = C[N/2 * N + N/2];
    }

    free(A); free(B); free(C);
    return total_us / iterations;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_host> <server_port> [matrix_size]\n", argv[0]);
        fprintf(stderr, "\nThis benchmark tests distributed matrix multiplication.\n");
        fprintf(stderr, "Set up a 2-node cluster with:\n");
        fprintf(stderr, "  Server 1: ./dsm-server -p 9000 -n 8192 \\\n");
        fprintf(stderr, "            DSM_PAGE_RANGE_START=0 DSM_PAGE_RANGE_END=4096\n");
        fprintf(stderr, "  Server 2: ./dsm-server -p 9001 -n 8192 \\\n");
        fprintf(stderr, "            DSM_PAGE_RANGE_START=4096 DSM_PAGE_RANGE_END=8192 \\\n");
        fprintf(stderr, "            DSM_SEED_ADDR=127.0.0.1 DSM_SEED_PORT=9100\n");
        return 1;
    }

    const char *host = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    size_t N = (argc > 3) ? (size_t)atoi(argv[3]) : 256;
    int iterations = 3;

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║        Cluster Matrix Multiplication Benchmark               ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    printf("Configuration:\n");
    printf("  Server: %s:%u\n", host, port);
    printf("  Matrix size: %zux%zu (%zu KB per matrix)\n", N, N, N * N * sizeof(double) / 1024);
    printf("  Pages per matrix: %zu\n", (N * N * sizeof(double) + PAGE_SIZE - 1) / PAGE_SIZE);
    printf("  Iterations: %d\n\n", iterations);

    printf("Running raw memory baseline...\n");
    uint64_t raw_us = bench_raw_matmul(N, iterations);
    printf("  Raw memory: %lu us\n\n", raw_us);

    printf("Running DSM distributed benchmark...\n");

    size_t pages_per_matrix = (N * N * sizeof(double) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint16_t local_pages = pages_per_matrix * 3 + 64;
    if (local_pages > 4096) local_pages = 4096;
    uint16_t virtual_pages = 8192;

    char *small_cache = getenv("DSM_SMALL_CACHE");
    if (small_cache && atoi(small_cache)) {
        local_pages = pages_per_matrix / 2;
        if (local_pages < 32) local_pages = 32;
        printf("  [SMALL CACHE MODE: %u pages]\n", local_pages);
    }

    dsm_context_t *ctx = dsm_create_context(local_pages, virtual_pages);
    if (!ctx) {
        fprintf(stderr, "Failed to create DSM context\n");
        return 1;
    }

    ctx->sock_fd = connect_to_server(host, port);
    if (ctx->sock_fd < 0) {
        fprintf(stderr, "Failed to connect to %s:%u\n", host, port);
        dsm_destroy_context(ctx);
        return 1;
    }

    printf("  Local cache: %u pages (%u KB)\n", local_pages, local_pages * 4);
    printf("  Virtual pages: %u\n\n", virtual_pages);

    uint64_t dsm_us = bench_distributed_matmul(ctx, N, iterations);

    printf("\n┌────────────────────────────────────────────────────────────────┐\n");
    printf("│                         Results                                │\n");
    printf("├────────────────────────────────────────────────────────────────┤\n");
    printf("│  Raw Memory:     %10lu us                                 │\n", raw_us);
    printf("│  DSM Cluster:    %10lu us                                 │\n", dsm_us);
    printf("│  Overhead:       %10.2fx                                   │\n", (double)dsm_us / raw_us);
    printf("├────────────────────────────────────────────────────────────────┤\n");
    printf("│  DSM Stats:                                                    │\n");
    printf("│    Local hits:   %10lu                                     │\n", ctx->local_hits);
    printf("│    Remote fetch: %10lu                                     │\n", ctx->remote_fetches);
    printf("│    Evictions:    %10lu                                     │\n", ctx->evictions);
    printf("│    Hit rate:     %10.1f%%                                    │\n",
           100.0 * ctx->local_hits / (ctx->local_hits + ctx->remote_fetches + 1));
    printf("└────────────────────────────────────────────────────────────────┘\n");

    close(ctx->sock_fd);
    dsm_destroy_context(ctx);

    return 0;
}
