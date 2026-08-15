#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#include "dsm_gossip.h"

static int
send_msg(int fd, const gossip_msg_t *msg, const struct sockaddr_in *dest)
{
    size_t len = sizeof(gossip_header_t) +
                 msg->hdr.node_count * sizeof(gossip_node_info_t);

    ssize_t n = sendto(fd, msg, len, 0,
                       (const struct sockaddr *)dest, sizeof(*dest));
    return (n == (ssize_t)len) ? 0 : -1;
}

static void
fill_header(gossip_msg_t *msg, uint8_t type, const cluster_node_t *self)
{
    memset(msg, 0, sizeof(*msg));
    msg->hdr.magic = GOSSIP_MAGIC;
    msg->hdr.type = type;
    msg->hdr.sender_client_port = self->client_port;
    memcpy(msg->hdr.sender_id, self->id, NODE_ID_LEN);
    msg->hdr.sender_incarnation = self->incarnation;
    msg->hdr.sender_range_start = self->range_start;
    msg->hdr.sender_range_end = self->range_end;
}

void
cluster_node_to_gossip(const cluster_node_t *cn, gossip_node_info_t *gn)
{
    memset(gn, 0, sizeof(*gn));
    memcpy(gn->id, cn->id, NODE_ID_LEN);
    gn->addr = cn->addr;
    gn->client_port = cn->client_port;
    gn->cluster_port = cn->cluster_port;
    gn->range_start = cn->range_start;
    gn->range_end = cn->range_end;
    gn->incarnation = cn->incarnation;
    gn->state = cn->state;
}

void
gossip_node_to_cluster(const gossip_node_info_t *gn, cluster_node_t *cn)
{
    memset(cn, 0, sizeof(*cn));
    memcpy(cn->id, gn->id, NODE_ID_LEN);
    cn->addr = gn->addr;
    cn->client_port = gn->client_port;
    cn->cluster_port = gn->cluster_port;
    cn->range_start = gn->range_start;
    cn->range_end = gn->range_end;
    cn->incarnation = gn->incarnation;
    cn->state = gn->state;
}

int
gossip_send_ping(int fd, const cluster_node_t *self, const struct sockaddr_in *dest)
{
    gossip_msg_t msg;
    fill_header(&msg, GOSSIP_PING, self);
    msg.hdr.node_count = 0;
    return send_msg(fd, &msg, dest);
}

int
gossip_send_pong(int fd, const cluster_node_t *self, const struct sockaddr_in *dest,
                 const cluster_node_t *nodes, int node_count)
{
    gossip_msg_t msg;
    fill_header(&msg, GOSSIP_PONG, self);

    int count = (node_count > GOSSIP_MSG_MAX_NODES) ? GOSSIP_MSG_MAX_NODES : node_count;
    msg.hdr.node_count = (uint8_t)count;

    for (int i = 0; i < count; i++)
        cluster_node_to_gossip(&nodes[i], &msg.nodes[i]);

    return send_msg(fd, &msg, dest);
}

int
gossip_send_join(int fd, const cluster_node_t *self, const struct sockaddr_in *dest)
{
    gossip_msg_t msg;
    fill_header(&msg, GOSSIP_JOIN, self);
    msg.hdr.node_count = 1;
    cluster_node_to_gossip(self, &msg.nodes[0]);
    return send_msg(fd, &msg, dest);
}

int
gossip_parse(const void *buf, size_t len, gossip_msg_t *out)
{
    if (len < sizeof(gossip_header_t))
        return -1;

    memcpy(&out->hdr, buf, sizeof(gossip_header_t));

    if (out->hdr.magic != GOSSIP_MAGIC)
        return -1;

    if (out->hdr.node_count > GOSSIP_MSG_MAX_NODES)
        return -1;

    size_t expected = sizeof(gossip_header_t) +
                      out->hdr.node_count * sizeof(gossip_node_info_t);
    if (len < expected)
        return -1;

    if (out->hdr.node_count > 0) {
        memcpy(out->nodes, (const uint8_t *)buf + sizeof(gossip_header_t),
               out->hdr.node_count * sizeof(gossip_node_info_t));
    }

    return 0;
}
