#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "dsm_cluster.h"
#include "dsm_gossip.h"
#include "dsm_protocol.h"

struct cluster_ctx {
    cluster_config_t cfg;
    cluster_state_t  state;
    cluster_node_t   self;

    int              udp_fd;
    volatile int     running;

    pthread_t        gossip_thread;
    pthread_t        heartbeat_thread;

    int              peer_fds[CLUSTER_MAX_NODES];
};

static uint64_t
now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void
generate_node_id(uint8_t *id)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, id, NODE_ID_LEN);
        close(fd);
    } else {
        uint64_t t = now_ms();
        memcpy(id, &t, sizeof(t));
        for (int i = 8; i < NODE_ID_LEN; i++)
            id[i] = (uint8_t)(rand() & 0xFF);
    }
}

static int
find_node_by_id(cluster_state_t *s, const uint8_t *id)
{
    for (int i = 0; i < s->node_count; i++) {
        if (memcmp(s->nodes[i].id, id, NODE_ID_LEN) == 0)
            return i;
    }
    return -1;
}

static void
merge_node(cluster_state_t *s, const cluster_node_t *node)
{
    int idx = find_node_by_id(s, node->id);

    if (idx >= 0) {
        cluster_node_t *existing = &s->nodes[idx];
        if (node->incarnation > existing->incarnation) {
            existing->addr = node->addr;
            existing->client_port = node->client_port;
            existing->cluster_port = node->cluster_port;
            existing->range_start = node->range_start;
            existing->range_end = node->range_end;
            existing->incarnation = node->incarnation;
            existing->state = node->state;
            existing->last_seen = now_ms();
        } else if (node->incarnation == existing->incarnation) {
            existing->last_seen = now_ms();
            if (node->state == NODE_STATE_ALIVE)
                existing->state = NODE_STATE_ALIVE;
        }
    } else if (s->node_count < CLUSTER_MAX_NODES) {
        s->nodes[s->node_count] = *node;
        s->nodes[s->node_count].last_seen = now_ms();
        s->node_count++;
    }
}

cluster_config_t
cluster_config_default(void)
{
    return (cluster_config_t){
        .seed_addr = NULL,
        .seed_port = DEFAULT_PORT + CLUSTER_PORT_OFFSET,
        .node_id = NULL,
        .cluster_port = DEFAULT_PORT + CLUSTER_PORT_OFFSET,
        .heartbeat_ms = HEARTBEAT_INTERVAL_MS,
        .gossip_ms = GOSSIP_INTERVAL_MS,
        .timeout_ms = HEARTBEAT_TIMEOUT_MS,
        .page_range_start = 0,
        .page_range_end = 0
    };
}

int
cluster_config_from_env(cluster_config_t *cfg)
{
    char *v;
    if ((v = getenv("DSM_SEED_ADDR"))) cfg->seed_addr = v;
    if ((v = getenv("DSM_SEED_PORT"))) cfg->seed_port = (uint16_t)atoi(v);
    if ((v = getenv("DSM_NODE_ID"))) cfg->node_id = v;
    if ((v = getenv("DSM_CLUSTER_PORT"))) cfg->cluster_port = (uint16_t)atoi(v);
    if ((v = getenv("DSM_HEARTBEAT_MS"))) cfg->heartbeat_ms = (uint32_t)atoi(v);
    if ((v = getenv("DSM_GOSSIP_MS"))) cfg->gossip_ms = (uint32_t)atoi(v);
    if ((v = getenv("DSM_TIMEOUT_MS"))) cfg->timeout_ms = (uint32_t)atoi(v);
    if ((v = getenv("DSM_PAGE_RANGE_START"))) cfg->page_range_start = (uint32_t)atoi(v);
    if ((v = getenv("DSM_PAGE_RANGE_END"))) cfg->page_range_end = (uint32_t)atoi(v);
    return 0;
}

void
cluster_config_print(const cluster_config_t *cfg)
{
    printf("Cluster Configuration:\n");
    printf("  Seed:          %s:%u\n", cfg->seed_addr ? cfg->seed_addr : "(none)", cfg->seed_port);
    printf("  Cluster Port:  %u\n", cfg->cluster_port);
    printf("  Heartbeat:     %u ms\n", cfg->heartbeat_ms);
    printf("  Gossip:        %u ms\n", cfg->gossip_ms);
    printf("  Timeout:       %u ms\n", cfg->timeout_ms);
    printf("  Page Range:    %u - %u\n", cfg->page_range_start, cfg->page_range_end);
}

cluster_ctx_t *
cluster_create(const cluster_config_t *cfg, uint32_t local_addr, uint16_t client_port)
{
    cluster_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->cfg = *cfg;
    pthread_rwlock_init(&ctx->state.lock, NULL);

    for (int i = 0; i < CLUSTER_MAX_NODES; i++)
        ctx->peer_fds[i] = -1;

    if (cfg->node_id) {
        size_t len = strlen(cfg->node_id);
        if (len > NODE_ID_LEN) len = NODE_ID_LEN;
        memcpy(ctx->self.id, cfg->node_id, len);
    } else {
        generate_node_id(ctx->self.id);
    }

    ctx->self.addr = local_addr;
    ctx->self.client_port = client_port;
    ctx->self.cluster_port = cfg->cluster_port;
    ctx->self.range_start = cfg->page_range_start;
    ctx->self.range_end = cfg->page_range_end;
    ctx->self.incarnation = 1;
    ctx->self.state = NODE_STATE_ALIVE;
    ctx->self.last_seen = now_ms();

    ctx->state.nodes[0] = ctx->self;
    ctx->state.node_count = 1;
    ctx->state.self_idx = 0;

    return ctx;
}

void
cluster_destroy(cluster_ctx_t *ctx)
{
    if (!ctx) return;
    cluster_stop(ctx);
    if (ctx->udp_fd >= 0) close(ctx->udp_fd);
    for (int i = 0; i < CLUSTER_MAX_NODES; i++) {
        if (ctx->peer_fds[i] >= 0) close(ctx->peer_fds[i]);
    }
    pthread_rwlock_destroy(&ctx->state.lock);
    free(ctx);
}

static void
handle_gossip_msg(cluster_ctx_t *ctx, const gossip_msg_t *msg, const struct sockaddr_in *from)
{
    cluster_node_t sender = {0};
    memcpy(sender.id, msg->hdr.sender_id, NODE_ID_LEN);
    sender.addr = from->sin_addr.s_addr;
    sender.client_port = msg->hdr.sender_client_port;
    sender.cluster_port = ntohs(from->sin_port);
    sender.range_start = msg->hdr.sender_range_start;
    sender.range_end = msg->hdr.sender_range_end;
    sender.incarnation = msg->hdr.sender_incarnation;
    sender.state = NODE_STATE_ALIVE;

    pthread_rwlock_wrlock(&ctx->state.lock);
    merge_node(&ctx->state, &sender);
    for (int i = 0; i < msg->hdr.node_count; i++) {
        cluster_node_t node;
        gossip_node_to_cluster(&msg->nodes[i], &node);
        merge_node(&ctx->state, &node);
    }
    pthread_rwlock_unlock(&ctx->state.lock);

    if (msg->hdr.type == GOSSIP_PING || msg->hdr.type == GOSSIP_JOIN) {
        pthread_rwlock_rdlock(&ctx->state.lock);
        gossip_send_pong(ctx->udp_fd, &ctx->self, from,
                        ctx->state.nodes, ctx->state.node_count);
        pthread_rwlock_unlock(&ctx->state.lock);
    }
}

static void *
gossip_loop(void *arg)
{
    cluster_ctx_t *ctx = arg;
    uint8_t buf[sizeof(gossip_msg_t)];
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(ctx->udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (ctx->running) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(ctx->udp_fd, buf, sizeof(buf), 0,
                            (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            gossip_msg_t msg;
            if (gossip_parse(buf, n, &msg) == 0)
                handle_gossip_msg(ctx, &msg, &from);
        }
    }
    return NULL;
}

static void *
heartbeat_loop(void *arg)
{
    cluster_ctx_t *ctx = arg;
    uint64_t last_gossip = 0;

    while (ctx->running) {
        usleep(ctx->cfg.heartbeat_ms * 1000 / 4);
        uint64_t now = now_ms();

        pthread_rwlock_wrlock(&ctx->state.lock);
        for (int i = 0; i < ctx->state.node_count; i++) {
            if (i == ctx->state.self_idx) continue;
            cluster_node_t *n = &ctx->state.nodes[i];
            uint64_t elapsed = now - n->last_seen;

            if (n->state == NODE_STATE_ALIVE && elapsed > ctx->cfg.timeout_ms / 2)
                n->state = NODE_STATE_SUSPECT;
            else if (n->state == NODE_STATE_SUSPECT && elapsed > ctx->cfg.timeout_ms)
                n->state = NODE_STATE_DEAD;
        }
        pthread_rwlock_unlock(&ctx->state.lock);

        if (now - last_gossip >= ctx->cfg.gossip_ms) {
            last_gossip = now;
            pthread_rwlock_rdlock(&ctx->state.lock);
            int count = ctx->state.node_count;
            int sent = 0;
            for (int i = 0; i < count && sent < GOSSIP_FANOUT; i++) {
                if (i == ctx->state.self_idx) continue;
                cluster_node_t *n = &ctx->state.nodes[i];
                if (n->state == NODE_STATE_DEAD) continue;

                struct sockaddr_in dest = {
                    .sin_family = AF_INET,
                    .sin_addr.s_addr = n->addr,
                    .sin_port = htons(n->cluster_port)
                };
                gossip_send_ping(ctx->udp_fd, &ctx->self, &dest);
                sent++;
            }
            pthread_rwlock_unlock(&ctx->state.lock);
        }
    }
    return NULL;
}

int
cluster_start(cluster_ctx_t *ctx)
{
    ctx->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->udp_fd < 0) return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(ctx->cfg.cluster_port)
    };

    if (bind(ctx->udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(ctx->udp_fd);
        ctx->udp_fd = -1;
        return -1;
    }

    ctx->running = 1;
    pthread_create(&ctx->gossip_thread, NULL, gossip_loop, ctx);
    pthread_create(&ctx->heartbeat_thread, NULL, heartbeat_loop, ctx);
    return 0;
}

void
cluster_stop(cluster_ctx_t *ctx)
{
    if (!ctx->running) return;
    ctx->running = 0;
    pthread_join(ctx->gossip_thread, NULL);
    pthread_join(ctx->heartbeat_thread, NULL);
}

int
cluster_join(cluster_ctx_t *ctx)
{
    if (!ctx->cfg.seed_addr) return 0;

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", ctx->cfg.seed_port);

    if (getaddrinfo(ctx->cfg.seed_addr, port_str, &hints, &res) != 0)
        return -1;

    struct sockaddr_in *seed = (struct sockaddr_in *)res->ai_addr;
    gossip_send_join(ctx->udp_fd, &ctx->self, seed);
    freeaddrinfo(res);

    for (int i = 0; i < 10 && ctx->state.node_count <= 1; i++)
        usleep(100000);

    return (ctx->state.node_count > 1) ? 0 : -1;
}

uint8_t
cluster_node_count(cluster_ctx_t *ctx)
{
    pthread_rwlock_rdlock(&ctx->state.lock);
    uint8_t count = ctx->state.node_count;
    pthread_rwlock_unlock(&ctx->state.lock);
    return count;
}

int
cluster_get_owner(cluster_ctx_t *ctx, uint32_t page_id, cluster_node_t *out)
{
    pthread_rwlock_rdlock(&ctx->state.lock);
    for (int i = 0; i < ctx->state.node_count; i++) {
        cluster_node_t *n = &ctx->state.nodes[i];
        if (n->state != NODE_STATE_DEAD &&
            page_id >= n->range_start && page_id < n->range_end) {
            *out = *n;
            pthread_rwlock_unlock(&ctx->state.lock);
            return 0;
        }
    }
    pthread_rwlock_unlock(&ctx->state.lock);
    return -1;
}

bool
cluster_is_local(cluster_ctx_t *ctx, uint32_t page_id)
{
    return page_id >= ctx->self.range_start && page_id < ctx->self.range_end;
}

void
cluster_get_self(cluster_ctx_t *ctx, cluster_node_t *out)
{
    *out = ctx->self;
}

int
cluster_get_nodes(cluster_ctx_t *ctx, cluster_node_t *out, int max_nodes)
{
    pthread_rwlock_rdlock(&ctx->state.lock);
    int count = (ctx->state.node_count < max_nodes) ? ctx->state.node_count : max_nodes;
    memcpy(out, ctx->state.nodes, count * sizeof(cluster_node_t));
    pthread_rwlock_unlock(&ctx->state.lock);
    return count;
}

static int
get_peer_connection(cluster_ctx_t *ctx, const cluster_node_t *node)
{
    int idx = -1;
    pthread_rwlock_rdlock(&ctx->state.lock);
    for (int i = 0; i < ctx->state.node_count; i++) {
        if (memcmp(ctx->state.nodes[i].id, node->id, NODE_ID_LEN) == 0) {
            idx = i;
            break;
        }
    }
    pthread_rwlock_unlock(&ctx->state.lock);

    if (idx < 0) return -1;
    if (ctx->peer_fds[idx] >= 0) return ctx->peer_fds[idx];

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = node->addr,
        .sin_port = htons(node->client_port)
    };

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    dsm_setup_socket_optimal(fd);
    ctx->peer_fds[idx] = fd;
    return fd;
}

int
cluster_forward_get(cluster_ctx_t *ctx, uint32_t page_id, void *buf)
{
    cluster_node_t owner;
    if (cluster_get_owner(ctx, page_id, &owner) < 0)
        return -1;

    int fd = get_peer_connection(ctx, &owner);
    if (fd < 0) return -1;

    dsm_rpc_header req = { .op = OP_GET_PAGE, .page_id = page_id };
    if (dsm_send_full(fd, &req, sizeof(req)) < 0)
        return -1;
    if (dsm_recv_full(fd, buf, PAGE_SIZE) < 0)
        return -1;

    return 0;
}

int
cluster_forward_put(cluster_ctx_t *ctx, uint32_t page_id, const void *buf)
{
    cluster_node_t owner;
    if (cluster_get_owner(ctx, page_id, &owner) < 0)
        return -1;

    int fd = get_peer_connection(ctx, &owner);
    if (fd < 0) return -1;

    dsm_rpc_header req = { .op = OP_PUT_PAGE, .page_id = page_id };
    if (dsm_send_full(fd, &req, sizeof(req)) < 0)
        return -1;
    if (dsm_send_full(fd, buf, PAGE_SIZE) < 0)
        return -1;

    return 0;
}

