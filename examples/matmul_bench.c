// benchmark comparing dsm page access overhead vs raw memory in matrix multiply

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "dsm_array.h"
#include "dsm_protocol.h"

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
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

static volatile double sink;

static uint64_t bench_raw_matmul(size_t N, int iterations)
{
    double *A = malloc(N * N * sizeof(double));
    double *B = malloc(N * N * sizeof(double));
    double *C = malloc(N * N * sizeof(double));

    for (size_t i = 0; i < N * N; i++) {
        A[i] = (double)(i % 100) / 100.0;
        B[i] = (double)((i * 7) % 100) / 100.0;
    }

    uint64_t total_cycles = 0;

    for (int iter = 0; iter < iterations; iter++) {
        memset(C, 0, N * N * sizeof(double));

        uint64_t start = rdtsc();

        for (size_t i = 0; i < N; i++) {
            for (size_t k = 0; k < N; k++) {
                double a_ik = A[i * N + k];
                for (size_t j = 0; j < N; j++) {
                    C[i * N + j] += a_ik * B[k * N + j];
                }
            }
        }

        uint64_t end = rdtsc();
        total_cycles += (end - start);

        sink = C[N/2 * N + N/2];
    }

    free(A); free(B); free(C);
    return total_cycles / iterations;
}

static uint64_t bench_dsm_matmul(dsm_context_t *ctx, size_t N, int iterations,
                                  uint64_t *out_hits, uint64_t *out_fetches)
{
    size_t shape[2] = {N, N};
    
    dsm_array_t *A = dsm_array_create(ctx, DSM_DOUBLE, 2, shape);
    dsm_array_t *B = dsm_array_create(ctx, DSM_DOUBLE, 2, shape);
    dsm_array_t *C = dsm_array_create(ctx, DSM_DOUBLE, 2, shape);
    
    if (!A || !B || !C) {
        fprintf(stderr, "Failed to create DSM arrays\n");
        return 0;
    }
    
    for (size_t i = 0; i < N; i++) {
        for (size_t j = 0; j < N; j++) {
            size_t idx = i * N + j;
            dsm_array_set_f64(A, (double)(idx % 100) / 100.0, i, j);
            dsm_array_set_f64(B, (double)((idx * 7) % 100) / 100.0, i, j);
        }
    }

    ctx->local_hits = 0;
    ctx->remote_fetches = 0;
    
    uint64_t total_cycles = 0;
    
    for (int iter = 0; iter < iterations; iter++) {
        for (size_t i = 0; i < N; i++) {
            for (size_t j = 0; j < N; j++) {
                dsm_array_set_f64(C, 0.0, i, j);
            }
        }
        
        uint64_t start = rdtsc();
        
        for (size_t i = 0; i < N; i++) {
            for (size_t k = 0; k < N; k++) {
                double a_ik = dsm_array_get_f64(A, i, k);
                for (size_t j = 0; j < N; j++) {
                    double c_ij = dsm_array_get_f64(C, i, j);
                    double b_kj = dsm_array_get_f64(B, k, j);
                    dsm_array_set_f64(C, c_ij + a_ik * b_kj, i, j);
                }
            }
        }
        
        uint64_t end = rdtsc();
        total_cycles += (end - start);
    }
    
    *out_hits = ctx->local_hits;
    *out_fetches = ctx->remote_fetches;
    
    dsm_array_free(A);
    dsm_array_free(B);
    dsm_array_free(C);
    
    return total_cycles / iterations;
}

static uint64_t bench_dsm_page_matmul(dsm_context_t *ctx, size_t N, int iterations,
                                       uint64_t *out_hits, uint64_t *out_fetches)
{
    size_t bytes_per_matrix = N * N * sizeof(double);
    size_t pages_per_matrix = (bytes_per_matrix + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t elems_per_page = PAGE_SIZE / sizeof(double);

    uint16_t base_A = ctx->next_alloc_page;
    ctx->next_alloc_page += pages_per_matrix;
    uint16_t base_B = ctx->next_alloc_page;
    ctx->next_alloc_page += pages_per_matrix;
    uint16_t base_C = ctx->next_alloc_page;
    ctx->next_alloc_page += pages_per_matrix;

    for (size_t i = 0; i < N * N; i++) {
        uint16_t page_a = base_A + (i / elems_per_page);
        size_t off_a = (i % elems_per_page) * sizeof(double);
        double *pa = (double *)((char *)dsm_access_page(ctx, page_a, 1) + off_a);
        *pa = (double)(i % 100) / 100.0;

        uint16_t page_b = base_B + (i / elems_per_page);
        size_t off_b = (i % elems_per_page) * sizeof(double);
        double *pb = (double *)((char *)dsm_access_page(ctx, page_b, 1) + off_b);
        *pb = (double)((i * 7) % 100) / 100.0;
    }

    ctx->local_hits = 0;
    ctx->remote_fetches = 0;

    uint64_t total_cycles = 0;

    for (int iter = 0; iter < iterations; iter++) {
        for (size_t p = 0; p < pages_per_matrix; p++) {
            double *pc = dsm_access_page(ctx, base_C + p, 1);
            memset(pc, 0, PAGE_SIZE);
        }

        uint64_t start = rdtsc();

        for (size_t i = 0; i < N; i++) {
            for (size_t k = 0; k < N; k++) {
                size_t idx_a = i * N + k;
                uint16_t page_a = base_A + (idx_a / elems_per_page);
                size_t off_a = (idx_a % elems_per_page) * sizeof(double);
                double a_ik = *(double *)((char *)dsm_access_page(ctx, page_a, 0) + off_a);

                for (size_t j = 0; j < N; j++) {
                    size_t idx_b = k * N + j;
                    uint16_t page_b = base_B + (idx_b / elems_per_page);
                    size_t off_b = (idx_b % elems_per_page) * sizeof(double);
                    double b_kj = *(double *)((char *)dsm_access_page(ctx, page_b, 0) + off_b);

                    size_t idx_c = i * N + j;
                    uint16_t page_c = base_C + (idx_c / elems_per_page);
                    size_t off_c = (idx_c % elems_per_page) * sizeof(double);
                    double *c_ij = (double *)((char *)dsm_access_page(ctx, page_c, 1) + off_c);
                    *c_ij += a_ik * b_kj;
                }
            }
        }

        uint64_t end = rdtsc();
        total_cycles += (end - start);

        size_t mid = (N/2) * N + (N/2);
        uint16_t page_mid = base_C + (mid / elems_per_page);
        size_t off_mid = (mid % elems_per_page) * sizeof(double);
        sink = *(double *)((char *)dsm_access_page(ctx, page_mid, 0) + off_mid);
    }

    *out_hits = ctx->local_hits;
    *out_fetches = ctx->remote_fetches;

    return total_cycles / iterations;
}

static uint64_t bench_dsm_prefetch_matmul(dsm_context_t *ctx, size_t N, int iterations,
                                           uint64_t *out_hits, uint64_t *out_fetches)
{
    size_t bytes_per_matrix = N * N * sizeof(double);
    size_t pages_per_matrix = (bytes_per_matrix + PAGE_SIZE - 1) / PAGE_SIZE;

    uint16_t base_A = ctx->next_alloc_page;
    ctx->next_alloc_page += pages_per_matrix;
    uint16_t base_B = ctx->next_alloc_page;
    ctx->next_alloc_page += pages_per_matrix;
    uint16_t base_C = ctx->next_alloc_page;
    ctx->next_alloc_page += pages_per_matrix;

    double *A = dsm_access_page_fast(ctx, base_A, 1);
    double *B = dsm_access_page_fast(ctx, base_B, 1);
    double *C = dsm_access_page_fast(ctx, base_C, 1);

    for (size_t p = 0; p < pages_per_matrix; p++) {
        dsm_access_page_fast(ctx, base_A + p, 0);
        dsm_access_page_fast(ctx, base_B + p, 0);
        dsm_access_page_fast(ctx, base_C + p, 1);
    }

    for (size_t i = 0; i < N * N; i++) {
        A[i] = (double)(i % 100) / 100.0;
        B[i] = (double)((i * 7) % 100) / 100.0;
    }

    ctx->local_hits = 0;
    ctx->remote_fetches = 0;

    uint64_t total_cycles = 0;

    for (int iter = 0; iter < iterations; iter++) {
        memset(C, 0, N * N * sizeof(double));

        uint64_t start = rdtsc();

        for (size_t i = 0; i < N; i++) {
            for (size_t k = 0; k < N; k++) {
                double a_ik = A[i * N + k];
                for (size_t j = 0; j < N; j++) {
                    C[i * N + j] += a_ik * B[k * N + j];
                }
            }
        }

        uint64_t end = rdtsc();
        total_cycles += (end - start);

        sink = C[N/2 * N + N/2];
    }

    *out_hits = ctx->local_hits;
    *out_fetches = ctx->remote_fetches;

    return total_cycles / iterations;
}

static uint64_t bench_dsm_cached_matmul(dsm_context_t *ctx, size_t N, int iterations,
                                         uint64_t *out_hits, uint64_t *out_fetches)
{
    size_t bytes_per_matrix = N * N * sizeof(double);
    size_t pages_per_matrix = (bytes_per_matrix + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t elems_per_page = PAGE_SIZE / sizeof(double);

    uint16_t base_A = ctx->next_alloc_page;
    ctx->next_alloc_page += pages_per_matrix;
    uint16_t base_B = ctx->next_alloc_page;
    ctx->next_alloc_page += pages_per_matrix;
    uint16_t base_C = ctx->next_alloc_page;
    ctx->next_alloc_page += pages_per_matrix;

    double *cur_page_a = NULL, *cur_page_b = NULL;
    uint16_t cur_pid_a = 0xFFFF, cur_pid_b = 0xFFFF;

    for (size_t i = 0; i < N * N; i++) {
        uint16_t pid_a = base_A + (i / elems_per_page);
        if (pid_a != cur_pid_a) {
            cur_page_a = dsm_access_page_fast(ctx, pid_a, 1);
            cur_pid_a = pid_a;
        }
        cur_page_a[i % elems_per_page] = (double)(i % 100) / 100.0;

        uint16_t pid_b = base_B + (i / elems_per_page);
        if (pid_b != cur_pid_b) {
            cur_page_b = dsm_access_page_fast(ctx, pid_b, 1);
            cur_pid_b = pid_b;
        }
        cur_page_b[i % elems_per_page] = (double)((i * 7) % 100) / 100.0;
    }

    ctx->local_hits = 0;
    ctx->remote_fetches = 0;

    uint64_t total_cycles = 0;

    for (int iter = 0; iter < iterations; iter++) {
        for (size_t p = 0; p < pages_per_matrix; p++) {
            memset(dsm_access_page_fast(ctx, base_C + p, 1), 0, PAGE_SIZE);
        }

        uint64_t start = rdtsc();

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
                    double b_kj = page_b[idx_b % elems_per_page];

                    size_t idx_c = i * N + j;
                    uint16_t new_pid_c = base_C + (idx_c / elems_per_page);
                    if (new_pid_c != pid_c) {
                        page_c = dsm_access_page_fast(ctx, new_pid_c, 1);
                        pid_c = new_pid_c;
                    }
                    page_c[idx_c % elems_per_page] += a_ik * b_kj;
                }
            }
        }

        uint64_t end = rdtsc();
        total_cycles += (end - start);

        size_t mid = (N/2) * N + (N/2);
        sink = *((double *)dsm_access_page_fast(ctx, base_C + mid / elems_per_page, 0) + (mid % elems_per_page));
    }

    *out_hits = ctx->local_hits;
    *out_fetches = ctx->remote_fetches;

    return total_cycles / iterations;
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    uint16_t port = 9999;
    int use_server = 1;
    
    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);
    if (argc > 3 && strcmp(argv[3], "--local") == 0) use_server = 0;
    
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║     Matrix Multiplication Benchmark: DSM vs Raw Memory       ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    size_t sizes[] = {16, 32, 64, 128};
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    int iterations = 3;

    printf("┌────────┬─────────────────┬─────────────────┬─────────────────┬──────────┬──────────┐\n");
    printf("│  Size  │   Raw Memory    │  DSM Per-Elem   │  DSM Prefetch   │ Per-Elem │ Prefetch │\n");
    printf("│  NxN   │  (cycles)       │  (page lookup)  │  (raw ptrs)     │ Overhead │ Overhead │\n");
    printf("├────────┼─────────────────┼─────────────────┼─────────────────┼──────────┼──────────┤\n");

    dsm_context_t *ctx = NULL;

    if (use_server) {
        ctx = dsm_create_context(1024, 32768);
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
    }

    for (int s = 0; s < num_sizes; s++) {
        size_t N = sizes[s];

        uint64_t raw_cycles = bench_raw_matmul(N, iterations);

        uint64_t dsm_perelem_cycles = 0;
        uint64_t hits = 0, fetches = 0;
        if (ctx) {
            dsm_perelem_cycles = bench_dsm_page_matmul(ctx, N, iterations, &hits, &fetches);
        }

        uint64_t dsm_prefetch_cycles = 0;
        if (ctx) {
            dsm_prefetch_cycles = bench_dsm_prefetch_matmul(ctx, N, iterations, &hits, &fetches);
        }

        double perelem_overhead = (ctx && raw_cycles > 0) ?
                          (double)dsm_perelem_cycles / raw_cycles : 0.0;
        double prefetch_overhead = (ctx && raw_cycles > 0) ?
                          (double)dsm_prefetch_cycles / raw_cycles : 0.0;

        printf("│ %3zux%-3zu │ %15lu │ %15lu │ %15lu │  %6.1fx │  %6.2fx │\n",
               N, N, raw_cycles, dsm_perelem_cycles, dsm_prefetch_cycles, perelem_overhead, prefetch_overhead);
    }

    printf("└────────┴─────────────────┴─────────────────┴─────────────────┴──────────┴──────────┘\n");

    if (ctx) {
        printf("\nFinal stats: hits=%lu fetches=%lu evictions=%lu\n",
               ctx->local_hits, ctx->remote_fetches, ctx->evictions);
        close(ctx->sock_fd);
        dsm_destroy_context(ctx);
    }

    printf("\nNotes:\n");
    printf("  - Raw Memory: malloc'd arrays with direct pointer arithmetic\n");
    printf("  - DSM Per-Elem: dsm_access_page() call for each element access\n");
    printf("  - DSM Prefetch: Prefetch all pages, then use raw pointers (zero DSM overhead in loop)\n");
    printf("  - Overhead = DSM cycles / Raw cycles\n");

    return 0;
}

