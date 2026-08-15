/* Unit tests for gossip message encode/parse (dsm_gossip.c).
 * Plain C asserts, no framework. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dsm_gossip.h"

static cluster_node_t
make_node(uint8_t seed)
{
    cluster_node_t cn;
    memset(&cn, 0, sizeof(cn));
    for (int i = 0; i < NODE_ID_LEN; i++)
        cn.id[i] = (uint8_t)(seed + i);
    cn.addr = 0x0100007Fu + seed;
    cn.client_port = (uint16_t)(9000 + seed);
    cn.cluster_port = (uint16_t)(10000 + seed);
    cn.range_start = 1024u * seed;
    cn.range_end = 1024u * (seed + 1);
    cn.incarnation = 7u + seed;
    cn.state = NODE_STATE_ALIVE;
    return cn;
}

static void
assert_nodes_equal(const cluster_node_t *a, const cluster_node_t *b)
{
    assert(memcmp(a->id, b->id, NODE_ID_LEN) == 0);
    assert(a->addr == b->addr);
    assert(a->client_port == b->client_port);
    assert(a->cluster_port == b->cluster_port);
    assert(a->range_start == b->range_start);
    assert(a->range_end == b->range_end);
    assert(a->incarnation == b->incarnation);
    assert(a->state == b->state);
}

/* Build a wire buffer: header + node_count node infos. Returns byte length. */
static size_t
build_msg(gossip_msg_t *msg, const cluster_node_t *nodes, uint8_t node_count)
{
    memset(msg, 0, sizeof(*msg));
    msg->hdr.magic = GOSSIP_MAGIC;
    msg->hdr.type = GOSSIP_PONG;
    msg->hdr.node_count = node_count;
    msg->hdr.sender_client_port = 9001;
    msg->hdr.sender_incarnation = 1;
    msg->hdr.sender_range_start = 0;
    msg->hdr.sender_range_end = 1024;
    for (uint8_t i = 0; i < node_count; i++)
        cluster_node_to_gossip(&nodes[i], &msg->nodes[i]);
    return sizeof(gossip_header_t) + (size_t)node_count * sizeof(gossip_node_info_t);
}

static void
test_roundtrip(void)
{
    cluster_node_t in[2] = { make_node(1), make_node(2) };
    gossip_msg_t msg, out;
    size_t len = build_msg(&msg, in, 2);

    assert(gossip_parse(&msg, len, &out) == 0);
    assert(out.hdr.magic == GOSSIP_MAGIC);
    assert(out.hdr.type == GOSSIP_PONG);
    assert(out.hdr.node_count == 2);

    for (int i = 0; i < 2; i++) {
        cluster_node_t back;
        gossip_node_to_cluster(&out.nodes[i], &back);
        assert_nodes_equal(&in[i], &back);
    }
    printf("PASS test_roundtrip\n");
}

static void
test_max_nodes_accepted(void)
{
    cluster_node_t in[GOSSIP_MSG_MAX_NODES];
    for (int i = 0; i < GOSSIP_MSG_MAX_NODES; i++)
        in[i] = make_node((uint8_t)(i + 1));

    gossip_msg_t msg, out;
    size_t len = build_msg(&msg, in, GOSSIP_MSG_MAX_NODES);
    assert(gossip_parse(&msg, len, &out) == 0);
    assert(out.hdr.node_count == GOSSIP_MSG_MAX_NODES);
    printf("PASS test_max_nodes_accepted\n");
}

static void
test_truncated_rejected(void)
{
    cluster_node_t in[2] = { make_node(1), make_node(2) };
    gossip_msg_t msg, out;
    size_t len = build_msg(&msg, in, 2);

    /* One byte short of the declared payload. */
    assert(gossip_parse(&msg, len - 1, &out) == -1);
    /* Shorter than the header. */
    assert(gossip_parse(&msg, sizeof(gossip_header_t) - 1, &out) == -1);
    printf("PASS test_truncated_rejected\n");
}

static void
test_bad_magic_rejected(void)
{
    cluster_node_t in[1] = { make_node(1) };
    gossip_msg_t msg, out;
    size_t len = build_msg(&msg, in, 1);
    msg.hdr.magic = 0xDEADBEEF;
    assert(gossip_parse(&msg, len, &out) == -1);
    printf("PASS test_bad_magic_rejected\n");
}

static void
test_oversized_node_count_rejected(void)
{
    /* A message claiming node_count = 255 with a buffer large enough to
     * satisfy the length check. Without the bounds check, gossip_parse
     * would memcpy 255 node infos into out.nodes[GOSSIP_MSG_MAX_NODES],
     * overflowing the destination. */
    static uint8_t buf[sizeof(gossip_header_t) + 255 * sizeof(gossip_node_info_t)];
    memset(buf, 0, sizeof(buf));

    gossip_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = GOSSIP_MAGIC;
    hdr.type = GOSSIP_PONG;
    hdr.node_count = 255;
    memcpy(buf, &hdr, sizeof(hdr));

    gossip_msg_t out;
    assert(gossip_parse(buf, sizeof(buf), &out) == -1);

    /* Also just above the limit. */
    hdr.node_count = GOSSIP_MSG_MAX_NODES + 1;
    memcpy(buf, &hdr, sizeof(hdr));
    assert(gossip_parse(buf, sizeof(buf), &out) == -1);
    printf("PASS test_oversized_node_count_rejected\n");
}

int
main(void)
{
    test_roundtrip();
    test_max_nodes_accepted();
    test_truncated_rejected();
    test_bad_magic_rejected();
    test_oversized_node_count_rejected();
    printf("All gossip unit tests passed\n");
    return 0;
}
