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
#include "dsm_config.h"
#include "dsm_cluster.h"
#include "dsm_log.h"

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
        DSM_LOG_ERROR("socket: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        DSM_LOG_ERROR("setsockopt SO_REUSEADDR: %s", strerror(errno));
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
        DSM_LOG_ERROR("Invalid bind address: %s", cfg->bind_addr);
        close(listen_fd);
        return -1;
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        DSM_LOG_ERROR("bind: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 128) < 0) {
        DSM_LOG_ERROR("listen: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    DSM_LOG_INFO("DSM Server listening on %s:%d", cfg->bind_addr, cfg->port);
    return listen_fd;
}

#define MAX_CLIENTS 256

typedef struct {
    int fd;
} client_thread_arg_t;

typedef struct {
    pthread_t tid;
    int fd;
} client_slot_t;

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

    DSM_LOG_DEBUG("Client done: served=%lu written=%lu forwarded=%lu",
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

    dsm_parse_set_usage(print_usage, argv[0]);

    int opt;
    while ((opt = getopt_long(argc, argv, "p:b:n:vhs:S:c:r:i:", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': cfg.port = (uint16_t)dsm_parse_long(optarg, "-p/--port", 1, 65535); break;
        case 'b': cfg.bind_addr = optarg; break;
        case 'n': cfg.num_virtual_pages = (uint32_t)dsm_parse_long(optarg, "-n/--pages", 1, (long)UINT32_MAX); break;
        case 'v': cfg.verbose = true; break;
        case 's': ccfg.seed_addr = optarg; break;
        case 'S': ccfg.seed_port = (uint16_t)dsm_parse_long(optarg, "-S/--seed-port", 1, 65535); break;
        case 'c': ccfg.cluster_port = (uint16_t)dsm_parse_long(optarg, "-c/--cluster-port", 1, 65535); break;
        case 'r':
            if (sscanf(optarg, "%u:%u", &ccfg.page_range_start, &ccfg.page_range_end) != 2) {
                DSM_LOG_ERROR("Invalid range: %s", optarg);
                print_usage(argv[0]);
                return 1;
            }
            break;
        case 'i': ccfg.node_id = optarg; break;
        case 'h': print_usage(argv[0]); return 0;
        default: print_usage(argv[0]); return 1;
        }
    }

    dsm_log_verbose = cfg.verbose ? 1 : 0;

    struct sigaction sa = { .sa_handler = handle_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    g_storage_pages = cfg.num_virtual_pages;
    size_t storage_size = (size_t)g_storage_pages * PAGE_SIZE;
    g_storage = mmap(NULL, storage_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (g_storage == MAP_FAILED) {
        DSM_LOG_ERROR("mmap storage: %s", strerror(errno));
        return 1;
    }
    madvise(g_storage, storage_size, MADV_WILLNEED);

    if (ccfg.page_range_end > 0) {
        uint32_t local_addr = get_local_addr();
        g_cluster = cluster_create(&ccfg, local_addr, cfg.port);
        if (!g_cluster) {
            DSM_LOG_ERROR("Failed to create cluster context");
            munmap(g_storage, storage_size);
            return 1;
        }
        if (cluster_start(g_cluster) < 0) {
            DSM_LOG_ERROR("Failed to start cluster");
            munmap(g_storage, storage_size);
            return 1;
        }
        if (ccfg.seed_addr && cluster_join(g_cluster) < 0) {
            DSM_LOG_WARN("failed to join cluster via seed");
        }
        if (cfg.verbose) cluster_config_print(&ccfg);
    }

    if (cfg.verbose) dsm_server_config_print(&cfg);

    int listen_fd = start_server(&cfg);
    if (listen_fd < 0) {
        munmap(g_storage, storage_size);
        return 1;
    }

    size_t slot_cap = MAX_CLIENTS;
    size_t slot_count = 0;
    client_slot_t *slots = malloc(slot_cap * sizeof(*slots));
    if (!slots) {
        DSM_LOG_ERROR("malloc: %s", strerror(errno));
        close(listen_fd);
        munmap(g_storage, storage_size);
        return 1;
    }

    while (atomic_load(&running)) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            DSM_LOG_ERROR("accept: %s", strerror(errno));
            break;
        }

        dsm_setup_socket_optimal(client_fd);

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        DSM_LOG_DEBUG("Client from %s:%d", client_ip, ntohs(client_addr.sin_port));

        client_thread_arg_t *cta = malloc(sizeof(*cta));
        if (!cta) {
            DSM_LOG_ERROR("malloc: %s", strerror(errno));
            close(client_fd);
            continue;
        }
        cta->fd = client_fd;

        if (slot_count == slot_cap) {
            /* Reap finished client threads to free slots. */
            size_t kept = 0;
            for (size_t i = 0; i < slot_count; i++) {
                if (pthread_tryjoin_np(slots[i].tid, NULL) == 0)
                    close(slots[i].fd);
                else
                    slots[kept++] = slots[i];
            }
            slot_count = kept;
            if (slot_count == slot_cap) {
                client_slot_t *grown = realloc(slots, 2 * slot_cap * sizeof(*slots));
                if (!grown) {
                    DSM_LOG_ERROR("realloc: %s", strerror(errno));
                    close(client_fd);
                    free(cta);
                    continue;
                }
                slots = grown;
                slot_cap *= 2;
            }
        }

        pthread_t tid;
        int rc = pthread_create(&tid, NULL, handle_client_thread, cta);
        if (rc != 0) {
            DSM_LOG_ERROR("pthread_create: %s", strerror(rc));
            close(client_fd);
            free(cta);
            continue;
        }
        slots[slot_count].tid = tid;
        slots[slot_count].fd = client_fd;
        slot_count++;
    }

    close(listen_fd);
    for (size_t i = 0; i < slot_count; i++)
        shutdown(slots[i].fd, SHUT_RDWR);
    for (size_t i = 0; i < slot_count; i++) {
        pthread_join(slots[i].tid, NULL);
        close(slots[i].fd);
    }
    free(slots);
    if (g_cluster) cluster_destroy(g_cluster);
    munmap(g_storage, storage_size);
    DSM_LOG_INFO("Server shutdown");
    return 0;
}

