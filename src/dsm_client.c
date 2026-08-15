#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <x86intrin.h>

#include "dsm_protocol.h"
#include "dsm_paging.h"
#include "dsm_config.h"

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [OPTIONS]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -H, --host <host>         Server hostname (default: %s)\n", DEFAULT_HOST);
    fprintf(stderr, "  -p, --port <port>         Server port (default: %d)\n", DEFAULT_PORT);
    fprintf(stderr, "  -n, --num-pages <n>       Local cache size in pages (default: 256)\n");
    fprintf(stderr, "  -N, --num-virtual <n>     Total virtual pages (default: 1024)\n");
    fprintf(stderr, "  -i, --iterations <n>      Number of benchmark iterations (default: 100000)\n");
    fprintf(stderr, "  -l, --locality <pct>      Locality percentage 0-100 (default: 80)\n");
    fprintf(stderr, "  -w, --write-pct <pct>     Write percentage 0-100 (default: 10)\n");
    fprintf(stderr, "  -v, --verbose             Enable verbose output\n");
    fprintf(stderr, "  -t, --verify              Verify end-to-end round-trip and exit\n");
    fprintf(stderr, "  -h, --help                Show this help message\n");
    fprintf(stderr, "\nEnvironment variables:\n");
    fprintf(stderr, "  DSM_HOST              Server hostname\n");
    fprintf(stderr, "  DSM_PORT              Server port\n");
    fprintf(stderr, "  DSM_NUM_PAGES         Local cache size\n");
    fprintf(stderr, "  DSM_NUM_VIRTUAL_PAGES Total virtual pages\n");
    fprintf(stderr, "  DSM_NUM_ITERATIONS    Benchmark iterations\n");
    fprintf(stderr, "  DSM_LOCALITY_PERCENT  Locality percentage\n");
    fprintf(stderr, "  DSM_WRITE_PERCENT     Write percentage\n");
    fprintf(stderr, "  DSM_VERBOSE           Enable verbose (set to 1)\n");
    fprintf(stderr, "  DSM_VERIFY            Enable verify mode (set to 1)\n");
}

static int connect_to_server(const char *host, uint16_t port, int max_retries)
{
    struct addrinfo hints, *res, *rp;
    char port_str[16];
    int fd = -1;
    int retry_delay_ms = INITIAL_RETRY_DELAY_MS;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0)
                      ? AF_INET : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;

    snprintf(port_str, sizeof(port_str), "%u", port);

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }

    for (int attempt = 0; attempt < max_retries; attempt++) {
        for (rp = res; rp != NULL; rp = rp->ai_next) {
            fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd < 0)
                continue;

            if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
                if (dsm_setup_socket_optimal(fd) < 0) {
                    fprintf(stderr, "Warning: failed to optimize socket\n");
                }
                freeaddrinfo(res);
                return fd;
            }

            close(fd);
            fd = -1;
        }

        if (attempt < max_retries - 1) {
            fprintf(stderr, "Connection failed, retrying in %d ms...\n", retry_delay_ms);
            usleep(retry_delay_ms * 1000);
            retry_delay_ms *= 2;
        }
    }

    freeaddrinfo(res);
    fprintf(stderr, "Failed to connect after %d attempts\n", max_retries);
    return -1;
}

static void run_benchmark(dsm_context_t *ctx, const dsm_client_config_t *cfg)
{
    printf("=== DSM Benchmark ===\n");
    printf("Local cache: %u pages, Virtual pages: %u\n",
           cfg->num_pages, cfg->num_virtual_pages);
    printf("Iterations: %u, Locality: %u%%, Write: %u%%\n",
           cfg->num_iterations, cfg->locality_percent, cfg->write_percent);
    printf("---------------------\n");

    for (uint32_t i = 0; i < cfg->num_virtual_pages; i++) {
        ctx->page_table[i].location = (i < cfg->num_pages) ? LOC_LOCAL : LOC_REMOTE;
    }

    uint32_t lcg_state = 12345;
    uint64_t start_cycles = __rdtsc();

    for (uint32_t i = 0; i < cfg->num_iterations; i++) {
        lcg_state = lcg_state * 1103515245 + 12345;
        uint32_t rand_val = (lcg_state >> 16) & 0x7FFF;

        uint32_t page_id;
        if ((rand_val % 100) < cfg->locality_percent) {
            page_id = rand_val % cfg->num_pages;
        } else {
            page_id = cfg->num_pages + (rand_val % (cfg->num_virtual_pages - cfg->num_pages));
        }

        lcg_state = lcg_state * 1103515245 + 12345;
        uint32_t write_rand = (lcg_state >> 16) & 0x7FFF;
        int is_write = (write_rand % 100) < cfg->write_percent;

        void *page_ptr = dsm_access_page(ctx, page_id, is_write);
        if (!page_ptr && ctx->page_table[page_id].location == LOC_REMOTE) {
            fprintf(stderr, "Failed to access page %u\n", page_id);
            break;
        }

        if (is_write && page_ptr) {
            ((volatile uint8_t *)page_ptr)[0] = (uint8_t)i;
        } else if (page_ptr) {
            volatile uint8_t dummy = ((volatile uint8_t *)page_ptr)[0];
            (void)dummy;
        }
    }

    uint64_t end_cycles = __rdtsc();
    uint64_t total_cycles = end_cycles - start_cycles;
    uint64_t total_accesses = ctx->local_hits + ctx->remote_fetches;
    double hit_rate = (total_accesses > 0) ?
        (100.0 * ctx->local_hits / total_accesses) : 0.0;

    printf("\n=== Results ===\n");
    printf("Total cycles:    %lu\n", total_cycles);
    printf("Cycles/access:   %.2f\n",
           cfg->num_iterations > 0 ? (double)total_cycles / cfg->num_iterations : 0.0);
    printf("Local hits:      %lu\n", ctx->local_hits);
    printf("Remote fetches:  %lu\n", ctx->remote_fetches);
    printf("Evictions:       %lu\n", ctx->evictions);
    printf("Hit rate:        %.2f%%\n", hit_rate);
}

static int run_verify(dsm_context_t *ctx, const dsm_client_config_t *cfg)
{
    uint8_t expected[PAGE_SIZE];

    /* Pass 1: fill every virtual page with a page-dependent pattern.
     * num_virtual_pages > num_pages forces eviction and writeback. */
    for (uint32_t p = 0; p < cfg->num_virtual_pages; p++) {
        uint8_t *ptr = dsm_access_page(ctx, p, 1);
        if (!ptr) {
            fprintf(stderr, "VERIFY FAILED: cannot access page %u for write\n", p);
            return 1;
        }
        for (uint32_t i = 0; i < PAGE_SIZE; i++)
            ptr[i] = (uint8_t)((p * 2654435761u + i) & 0xFF);
    }

    /* Pass 2: re-read every page and compare against the pattern. */
    for (uint32_t p = 0; p < cfg->num_virtual_pages; p++) {
        uint8_t *ptr = dsm_access_page(ctx, p, 0);
        if (!ptr) {
            fprintf(stderr, "VERIFY FAILED: cannot access page %u for read\n", p);
            return 1;
        }
        for (uint32_t i = 0; i < PAGE_SIZE; i++)
            expected[i] = (uint8_t)((p * 2654435761u + i) & 0xFF);
        if (memcmp(ptr, expected, PAGE_SIZE) != 0) {
            fprintf(stderr, "VERIFY FAILED: page %u contents mismatch\n", p);
            return 1;
        }
    }

    printf("VERIFY OK %u pages\n", cfg->num_virtual_pages);
    return 0;
}

static struct option long_options[] = {
    {"host",        required_argument, 0, 'H'},
    {"port",        required_argument, 0, 'p'},
    {"num-pages",   required_argument, 0, 'n'},
    {"num-virtual", required_argument, 0, 'N'},
    {"iterations",  required_argument, 0, 'i'},
    {"locality",    required_argument, 0, 'l'},
    {"write-pct",   required_argument, 0, 'w'},
    {"verbose",     no_argument,       0, 'v'},
    {"verify",      no_argument,       0, 't'},
    {"help",        no_argument,       0, 'h'},
    {0, 0, 0, 0}
};

int main(int argc, char **argv)
{
    dsm_client_config_t cfg = dsm_client_config_default();
    dsm_client_config_from_env(&cfg);

    int opt;
    while ((opt = getopt_long(argc, argv, "H:p:n:N:i:l:w:vth", long_options, NULL)) != -1) {
        switch (opt) {
        case 'H':
            cfg.host = optarg;
            break;
        case 'p':
            cfg.port = (uint16_t)atoi(optarg);
            break;
        case 'n':
            cfg.num_pages = (uint32_t)atoi(optarg);
            break;
        case 'N':
            cfg.num_virtual_pages = (uint32_t)atoi(optarg);
            break;
        case 'i':
            cfg.num_iterations = (uint32_t)atoi(optarg);
            break;
        case 'l':
            cfg.locality_percent = (uint8_t)atoi(optarg);
            break;
        case 'w':
            cfg.write_percent = (uint8_t)atoi(optarg);
            break;
        case 'v':
            cfg.verbose = true;
            break;
        case 't':
            cfg.verify_mode = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (cfg.verify_mode) {
        /* Force a virtual range 4x the local cache so verification
         * exercises eviction, writeback, and refetch. */
        cfg.num_virtual_pages = 4u * cfg.num_pages;
    }

    if (cfg.locality_percent > 100) {
        fprintf(stderr, "Error: locality percentage must be 0-100\n");
        return 1;
    }
    if (cfg.write_percent > 100) {
        fprintf(stderr, "Error: write percentage must be 0-100\n");
        return 1;
    }
    if (cfg.num_pages == 0 || cfg.num_virtual_pages == 0) {
        fprintf(stderr, "Error: page counts must be > 0\n");
        return 1;
    }
    if (cfg.num_pages > cfg.num_virtual_pages) {
        fprintf(stderr, "Error: local pages cannot exceed virtual pages\n");
        return 1;
    }

    if (cfg.verbose) {
        dsm_client_config_print(&cfg);
    }

    int sock_fd = connect_to_server(cfg.host, cfg.port, MAX_RETRY_ATTEMPTS);
    if (sock_fd < 0) {
        fprintf(stderr, "Failed to connect to server %s:%u\n", cfg.host, cfg.port);
        return 1;
    }

    if (cfg.verbose) {
        printf("Connected to %s:%u\n", cfg.host, cfg.port);
    }

    dsm_context_t *ctx = dsm_create_context(cfg.num_pages, cfg.num_virtual_pages);
    if (!ctx) {
        fprintf(stderr, "Failed to create DSM context\n");
        close(sock_fd);
        return 1;
    }
    ctx->sock_fd = sock_fd;

    int exit_code = 0;
    if (cfg.verify_mode) {
        exit_code = run_verify(ctx, &cfg);
    } else {
        run_benchmark(ctx, &cfg);
    }

    if (cfg.verbose) {
        printf("\n=== Final Statistics ===\n");
        printf("Local hits:     %lu\n", ctx->local_hits);
        printf("Remote fetches: %lu\n", ctx->remote_fetches);
        printf("Evictions:      %lu\n", ctx->evictions);
    }

    close(sock_fd);
    dsm_destroy_context(ctx);

    return exit_code;
}
