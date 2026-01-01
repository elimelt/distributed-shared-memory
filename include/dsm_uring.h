#ifndef DSM_URING_H
#define DSM_URING_H

#include <stdint.h>
#include <stdbool.h>
#include <liburing.h>
#include "dsm_protocol.h"

#define URING_QUEUE_DEPTH 256
#define URING_MAX_CLIENTS 64

typedef enum {
    URING_OP_ACCEPT,
    URING_OP_RECV_HEADER,
    URING_OP_RECV_PAGE,
    URING_OP_SEND_PAGE,
    URING_OP_SEND_ACK
} uring_op_type_t;

typedef struct {
    int fd;
    int state;
    dsm_rpc_header req;
    uint8_t page_buf[PAGE_SIZE] __attribute__((aligned(64)));
    uint64_t pages_served;
    uint64_t pages_written;
    uint64_t pages_forwarded;
} uring_client_t;

typedef struct {
    uring_op_type_t op;
    int client_idx;
    uint32_t page_id;
} uring_event_t;

typedef struct {
    struct io_uring ring;
    int listen_fd;
    uring_client_t clients[URING_MAX_CLIENTS];
    uring_event_t events[URING_QUEUE_DEPTH];
    int event_idx;
    uint8_t *storage;
    uint32_t storage_pages;
    bool running;
} uring_server_t;

int uring_server_init(uring_server_t *srv, int listen_fd,
                      uint8_t *storage, uint32_t storage_pages);
void uring_server_destroy(uring_server_t *srv);
int uring_server_run(uring_server_t *srv);
void uring_server_stop(uring_server_t *srv);
int uring_queue_accept(uring_server_t *srv);
int uring_queue_recv_header(uring_server_t *srv, int client_idx);
int uring_queue_recv_page(uring_server_t *srv, int client_idx);
int uring_queue_send_page(uring_server_t *srv, int client_idx, void *data);

#endif /* DSM_URING_H */

