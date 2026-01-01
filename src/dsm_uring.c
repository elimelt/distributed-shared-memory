#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "dsm_uring.h"
#include "dsm_protocol.h"

static uring_event_t *get_event(uring_server_t *srv, uring_op_type_t op, int idx) {
    uring_event_t *ev = &srv->events[srv->event_idx];
    srv->event_idx = (srv->event_idx + 1) % URING_QUEUE_DEPTH;
    ev->op = op; ev->client_idx = idx; ev->page_id = 0;
    return ev;
}

static int find_free_client(uring_server_t *srv) {
    for (int i = 0; i < URING_MAX_CLIENTS; i++)
        if (srv->clients[i].fd < 0) return i;
    return -1;
}

int uring_server_init(uring_server_t *srv, int listen_fd, uint8_t *storage, uint32_t pages) {
    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = listen_fd; srv->storage = storage; srv->storage_pages = pages;
    srv->running = true; srv->event_idx = 0;
    for (int i = 0; i < URING_MAX_CLIENTS; i++) srv->clients[i].fd = -1;
    struct io_uring_params params = {0};
    params.flags = IORING_SETUP_SQPOLL; params.sq_thread_idle = 1000;
    int ret = io_uring_queue_init_params(URING_QUEUE_DEPTH, &srv->ring, &params);
    if (ret < 0) {
        memset(&params, 0, sizeof(params));
        ret = io_uring_queue_init_params(URING_QUEUE_DEPTH, &srv->ring, &params);
        if (ret < 0) { fprintf(stderr, "io_uring init failed\n"); return -1; }
        printf("[io_uring] regular mode\n");
    } else { printf("[io_uring] SQPOLL mode\n"); }
    return 0;
}

void uring_server_destroy(uring_server_t *srv) {
    for (int i = 0; i < URING_MAX_CLIENTS; i++)
        if (srv->clients[i].fd >= 0) { close(srv->clients[i].fd); srv->clients[i].fd = -1; }
    io_uring_queue_exit(&srv->ring);
}

void uring_server_stop(uring_server_t *srv) { srv->running = false; }

int uring_queue_accept(uring_server_t *srv) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&srv->ring);
    if (!sqe) return -1;
    io_uring_prep_accept(sqe, srv->listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, get_event(srv, URING_OP_ACCEPT, -1));
    return 0;
}

int uring_queue_recv_header(uring_server_t *srv, int idx) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&srv->ring);
    if (!sqe) return -1;
    uring_client_t *c = &srv->clients[idx];
    io_uring_prep_recv(sqe, c->fd, &c->req, sizeof(c->req), MSG_WAITALL);
    io_uring_sqe_set_data(sqe, get_event(srv, URING_OP_RECV_HEADER, idx));
    return 0;
}

int uring_queue_recv_page(uring_server_t *srv, int idx) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&srv->ring);
    if (!sqe) return -1;
    uring_client_t *c = &srv->clients[idx];
    io_uring_prep_recv(sqe, c->fd, c->page_buf, PAGE_SIZE, MSG_WAITALL);
    io_uring_sqe_set_data(sqe, get_event(srv, URING_OP_RECV_PAGE, idx));
    return 0;
}

int uring_queue_send_page(uring_server_t *srv, int idx, void *data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&srv->ring);
    if (!sqe) return -1;
    uring_client_t *c = &srv->clients[idx];
    io_uring_prep_send(sqe, c->fd, data, PAGE_SIZE, MSG_NOSIGNAL);
    io_uring_sqe_set_data(sqe, get_event(srv, URING_OP_SEND_PAGE, idx));
    return 0;
}

static void handle_accept(uring_server_t *srv, int fd) {
    if (fd < 0) { uring_queue_accept(srv); return; }
    int idx = find_free_client(srv);
    if (idx < 0) { close(fd); uring_queue_accept(srv); return; }
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));
    uring_client_t *c = &srv->clients[idx];
    c->fd = fd; c->state = 0; c->pages_served = c->pages_written = c->pages_forwarded = 0;
    uring_queue_recv_header(srv, idx);
    uring_queue_accept(srv);
}

static uint8_t zero_page[PAGE_SIZE];

static void handle_recv_header(uring_server_t *srv, int idx, int res) {
    uring_client_t *c = &srv->clients[idx];
    if (res <= 0) { close(c->fd); c->fd = -1; return; }
    if (c->req.op == OP_GET_PAGE) {
        void *data = (c->req.page_id < srv->storage_pages)
            ? srv->storage + ((size_t)c->req.page_id * PAGE_SIZE) : zero_page;
        uring_queue_send_page(srv, idx, data);
        if (c->req.page_id < srv->storage_pages) c->pages_served++;
    } else if (c->req.op == OP_PUT_PAGE) {
        uring_queue_recv_page(srv, idx);
    } else { close(c->fd); c->fd = -1; }
}

static void handle_recv_page(uring_server_t *srv, int idx, int res) {
    uring_client_t *c = &srv->clients[idx];
    if (res <= 0) { close(c->fd); c->fd = -1; return; }
    if (c->req.page_id < srv->storage_pages) {
        memcpy(srv->storage + ((size_t)c->req.page_id * PAGE_SIZE), c->page_buf, PAGE_SIZE);
        c->pages_written++;
    }
    uring_queue_recv_header(srv, idx);
}

static void handle_send_page(uring_server_t *srv, int idx, int res) {
    uring_client_t *c = &srv->clients[idx];
    if (res <= 0) { close(c->fd); c->fd = -1; return; }
    uring_queue_recv_header(srv, idx);
}

int uring_server_run(uring_server_t *srv) {
    uring_queue_accept(srv);
    io_uring_submit(&srv->ring);

    while (srv->running) {
        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&srv->ring, &cqe);
        if (ret < 0) { if (ret == -EINTR) continue; break; }

        unsigned head, count = 0;
        io_uring_for_each_cqe(&srv->ring, head, cqe) {
            uring_event_t *ev = io_uring_cqe_get_data(cqe);
            int res = cqe->res;
            switch (ev->op) {
            case URING_OP_ACCEPT: handle_accept(srv, res); break;
            case URING_OP_RECV_HEADER: handle_recv_header(srv, ev->client_idx, res); break;
            case URING_OP_RECV_PAGE: handle_recv_page(srv, ev->client_idx, res); break;
            case URING_OP_SEND_PAGE: handle_send_page(srv, ev->client_idx, res); break;
            default: break;
            }
            count++;
        }
        io_uring_cq_advance(&srv->ring, count);
        io_uring_submit(&srv->ring);
    }

    printf("[io_uring] Shutdown\n");
    for (int i = 0; i < URING_MAX_CLIENTS; i++) {
        uring_client_t *c = &srv->clients[i];
        if (c->pages_served || c->pages_written)
            printf("  Client %d: served=%lu written=%lu\n", i, c->pages_served, c->pages_written);
    }
    return 0;
}
