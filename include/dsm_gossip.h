#ifndef DSM_GOSSIP_H
#define DSM_GOSSIP_H

#include <stdint.h>
#include "dsm_cluster.h"

#define GOSSIP_MSG_MAX_NODES  16
#define GOSSIP_MAGIC          0x44534D47

typedef enum {
    GOSSIP_PING = 1,
    GOSSIP_PONG = 2,
    GOSSIP_JOIN = 3
} gossip_msg_type_t;

typedef struct {
    uint32_t magic;
    uint8_t  type;
    uint8_t  node_count;
    uint16_t sender_client_port;
    uint8_t  sender_id[NODE_ID_LEN];
    uint32_t sender_incarnation;
    uint32_t sender_range_start;
    uint32_t sender_range_end;
} __attribute__((packed)) gossip_header_t;

typedef struct {
    uint8_t  id[NODE_ID_LEN];
    uint32_t addr;
    uint16_t client_port;
    uint16_t cluster_port;
    uint32_t range_start;
    uint32_t range_end;
    uint32_t incarnation;
    uint8_t  state;
    uint8_t  pad[3];
} __attribute__((packed)) gossip_node_info_t;

typedef struct {
    gossip_header_t    hdr;
    gossip_node_info_t nodes[GOSSIP_MSG_MAX_NODES];
} __attribute__((packed)) gossip_msg_t;

int gossip_send_ping(int fd, const cluster_node_t *self, const struct sockaddr_in *dest);
int gossip_send_pong(int fd, const cluster_node_t *self, const struct sockaddr_in *dest,
                     const cluster_node_t *nodes, int node_count);
int gossip_send_join(int fd, const cluster_node_t *self, const struct sockaddr_in *dest);

int gossip_parse(const void *buf, size_t len, gossip_msg_t *out);
void gossip_node_to_cluster(const gossip_node_info_t *gn, cluster_node_t *cn);
void cluster_node_to_gossip(const cluster_node_t *cn, gossip_node_info_t *gn);

#endif

