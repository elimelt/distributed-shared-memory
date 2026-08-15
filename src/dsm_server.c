#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#include "dsm_protocol.h"
#include "dsm_paging.h"
#include "dsm_config.h"
#include "dsm_cluster.h"

static atomic_int running = 1;
static cluster_ctx_t *g_cluster = NULL;
static uint8_t *g_storage = NULL;
static uint32_t g_storage_pages = 0;

static void handle_signal(int sig)
{
    (void)sig;
    atomic_store(&running, 0);
}

static uint32_t get_local_addr(void)
{
    struct ifaddrs *ifa, *ifa_tmp;
    uint32_t addr = htonl(INADDR_LOOPBACK);

    if (getifaddrs(&ifa) == -1) return addr;

    for (ifa_tmp = ifa; ifa_tmp; ifa_tmp = ifa_tmp->ifa_next) {
        if (!ifa_tmp->ifa_addr) continue;
        if (ifa_tmp->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa_tmp->ifa_name, "lo") == 0) continue;

        struct sockaddr_in *sa = (struct sockaddr_in *)ifa_tmp->ifa_addr;
        addr = sa->sin_addr.s_addr;
        break;
    }
    freeifaddrs(ifa);
    return addr;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nServer Options:\n");
    printf("  -p, --port PORT          Client port (default: %d)\n", DEFAULT_PORT);
    printf("  -b, --bind ADDR          Bind address (default: 0.0.0.0)\n");
    printf("  -n, --pages NUM          Virtual pages to manage\n");
    printf("  -v, --verbose            Verbose output\n");
    printf("  -h, --help               Show this message\n");
    printf("\nCluster Options:\n");
    printf("  -s, --seed ADDR          Seed node address to join\n");
    printf("  -S, --seed-port PORT     Seed node cluster port\n");
    printf("  -c, --cluster-port PORT  This node's cluster port\n");
    printf("  -r, --range START:END    Page range this node owns\n");
    printf("  -i, --node-id ID         Node identifier\n");
    printf("\nEnvironment: DSM_SEED_ADDR, DSM_CLUSTER_PORT, DSM_PAGE_RANGE_START, etc.\n");
}

static int start_server(const dsm_server_config_t *cfg)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(listen_fd);
        return -1;
    }

    int defer = 1;
    setsockopt(listen_fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer, sizeof(defer));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);
    if (inet_pton(AF_INET, cfg->bind_addr, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid bind address: %s\n", cfg->bind_addr);
        close(listen_fd);
        return -1;
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    printf("DSM Server listening on %s:%d\n", cfg->bind_addr, cfg->port);
    return listen_fd;
}

typedef struct {
    int fd;
    dsm_server_config_t cfg;
} client_thread_arg_t;

static void *handle_client_thread(void *arg)
{
    client_thread_arg_t *cta = arg;
    int client_fd = cta->fd;
    uint64_t pages_served = 0, pages_written = 0, pages_forwarded = 0;
    uint8_t page_buf[PAGE_SIZE] __attribute__((aligned(64)));
    static uint8_t zero_page[PAGE_SIZE] __attribute__((aligned(64)));

    while (atomic_load_explicit(&running, memory_order_relaxed)) {
        dsm_rpc_header_t req;
        if (__builtin_expect(dsm_recv_full(client_fd, &req, sizeof(req)) < 0, 0))
            break;

        bool is_local = !g_cluster || cluster_is_local(g_cluster, req.page_id);

        if (req.op == OP_GET_PAGE) {
            if (__builtin_expect(is_local && req.page_id < g_storage_pages, 1)) {
                void *data = g_storage + ((size_t)req.page_id * PAGE_SIZE);
                __builtin_prefetch(data + PAGE_SIZE, 0, 1);
                if (dsm_send_full(client_fd, data, PAGE_SIZE, 0) < 0) break;
                pages_served++;
            } else if (g_cluster) {
                if (cluster_forward_get(g_cluster, req.page_id, page_buf) == 0) {
                    pages_forwarded++;
                } else {
                    if (dsm_send_full(client_fd, zero_page, PAGE_SIZE, 0) < 0) break;
                    continue;
                }
                if (dsm_send_full(client_fd, page_buf, PAGE_SIZE, 0) < 0) break;
            } else {
                if (dsm_send_full(client_fd, zero_page, PAGE_SIZE, 0) < 0) break;
            }
        } else if (req.op == OP_PUT_PAGE) {
            if (dsm_recv_full(client_fd, page_buf, PAGE_SIZE) < 0)
                break;

            if (__builtin_expect(is_local && req.page_id < g_storage_pages, 1)) {
                void *dest = g_storage + ((size_t)req.page_id * PAGE_SIZE);
                memcpy(dest, page_buf, PAGE_SIZE);
                pages_written++;
            } else if (g_cluster) {
                /* ACK only a successful forward; failure is fatal for
                 * this client connection */
                if (cluster_forward_put(g_cluster, req.page_id, page_buf) < 0)
                    break;
                pages_forwarded++;
            } else {
                break;
            }

            dsm_rpc_header_t ack = dsm_rpc_make_header(OP_ACK, req.page_id);
            if (dsm_send_full(client_fd, &ack, sizeof(ack), 0) < 0) break;
        }
    }

    close(client_fd);
    printf("Client done: served=%lu written=%lu forwarded=%lu\n",
           (unsigned long)pages_served, (unsigned long)pages_written,
           (unsigned long)pages_forwarded);
    free(cta);
    return NULL;
}

int main(int argc, char **argv)
{
    dsm_server_config_t cfg = dsm_server_config_default();
    cluster_config_t ccfg = cluster_config_default();
    dsm_server_config_from_env(&cfg);
    cluster_config_from_env(&ccfg);

    static struct option long_opts[] = {
        {"port", required_argument, 0, 'p'},
        {"bind", required_argument, 0, 'b'},
        {"pages", required_argument, 0, 'n'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {"seed", required_argument, 0, 's'},
        {"seed-port", required_argument, 0, 'S'},
        {"cluster-port", required_argument, 0, 'c'},
        {"range", required_argument, 0, 'r'},
        {"node-id", required_argument, 0, 'i'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:b:n:vhs:S:c:r:i:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': cfg.port = (uint16_t)atoi(optarg); break;
        case 'b': cfg.bind_addr = optarg; break;
        case 'n': cfg.num_virtual_pages = (uint16_t)atoi(optarg); break;
        case 'v': cfg.verbose = true; break;
        case 's': ccfg.seed_addr = optarg; break;
        case 'S': ccfg.seed_port = (uint16_t)atoi(optarg); break;
        case 'c': ccfg.cluster_port = (uint16_t)atoi(optarg); break;
        case 'r': sscanf(optarg, "%u:%u", &ccfg.page_range_start, &ccfg.page_range_end); break;
        case 'i': ccfg.node_id = optarg; break;
        case 'h': print_usage(argv[0]); return 0;
        default: print_usage(argv[0]); return 1;
        }
    }

    struct sigaction sa = { .sa_handler = handle_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    g_storage_pages = cfg.num_virtual_pages;
    size_t storage_size = (size_t)g_storage_pages * PAGE_SIZE;
    g_storage = mmap(NULL, storage_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (g_storage == MAP_FAILED) {
        perror("mmap storage");
        return 1;
    }
    madvise(g_storage, storage_size, MADV_WILLNEED);

    if (ccfg.page_range_end > 0) {
        uint32_t local_addr = get_local_addr();
        g_cluster = cluster_create(&ccfg, local_addr, cfg.port);
        if (!g_cluster) {
            fprintf(stderr, "Failed to create cluster context\n");
            return 1;
        }
        if (cluster_start(g_cluster) < 0) {
            fprintf(stderr, "Failed to start cluster\n");
            return 1;
        }
        if (ccfg.seed_addr && cluster_join(g_cluster) < 0) {
            fprintf(stderr, "Warning: failed to join cluster via seed\n");
        }
        if (cfg.verbose) cluster_config_print(&ccfg);
    }

    if (cfg.verbose) dsm_server_config_print(&cfg);

    int listen_fd = start_server(&cfg);
    if (listen_fd < 0) return 1;

    while (atomic_load(&running)) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        dsm_setup_socket_optimal(client_fd);

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        if (cfg.verbose)
            printf("Client from %s:%d\n", client_ip, ntohs(client_addr.sin_port));

        client_thread_arg_t *cta = malloc(sizeof(*cta));
        cta->fd = client_fd;
        cta->cfg = cfg;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, handle_client_thread, cta);
        pthread_attr_destroy(&attr);
    }

    close(listen_fd);
    if (g_cluster) cluster_destroy(g_cluster);
    munmap(g_storage, storage_size);
    printf("Server shutdown\n");
    return 0;
}

