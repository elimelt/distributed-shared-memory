#ifndef DSM_CLUSTER_H
#define DSM_CLUSTER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <netinet/in.h>

#define CLUSTER_MAX_NODES      64
#define NODE_ID_LEN            16
#define CLUSTER_PORT_OFFSET    1000
#define HEARTBEAT_INTERVAL_MS  1000
#define HEARTBEAT_TIMEOUT_MS   3000
#define GOSSIP_INTERVAL_MS     500
#define GOSSIP_FANOUT          3

typedef enum {
    NODE_STATE_ALIVE = 1,
    NODE_STATE_SUSPECT = 2,
    NODE_STATE_DEAD = 3
} node_state_t;

typedef struct {
    uint8_t  id[NODE_ID_LEN];
    uint32_t addr;
    uint16_t client_port;
    uint16_t cluster_port;
    uint32_t range_start;
    uint32_t range_end;
    uint64_t last_seen;
    uint32_t incarnation;
    uint8_t  state;
    uint8_t  pad[3];
} __attribute__((packed)) cluster_node_t;

typedef struct {
    cluster_node_t nodes[CLUSTER_MAX_NODES];
    uint8_t        node_count;
    uint8_t        self_idx;
    uint8_t        pad[2];
    pthread_rwlock_t lock;
} cluster_state_t;

typedef struct {
    char     *seed_addr;
    uint16_t seed_port;
    char     *node_id;
    uint16_t cluster_port;
    uint32_t heartbeat_ms;
    uint32_t gossip_ms;
    uint32_t timeout_ms;
    uint32_t page_range_start;
    uint32_t page_range_end;
} cluster_config_t;

typedef struct cluster_ctx cluster_ctx_t;

/* cluster_config_t population and printing live in dsm_config.h. */

cluster_ctx_t *cluster_create(const cluster_config_t *cfg, uint32_t local_addr, uint16_t client_port);
void cluster_destroy(cluster_ctx_t *ctx);

int cluster_start(cluster_ctx_t *ctx);
void cluster_stop(cluster_ctx_t *ctx);

int cluster_join(cluster_ctx_t *ctx);
int cluster_get_owner(cluster_ctx_t *ctx, uint32_t page_id, cluster_node_t *out);
bool cluster_is_local(cluster_ctx_t *ctx, uint32_t page_id);

int cluster_forward_get(cluster_ctx_t *ctx, uint32_t page_id, void *buf);
int cluster_forward_put(cluster_ctx_t *ctx, uint32_t page_id, const void *buf);

#endif

