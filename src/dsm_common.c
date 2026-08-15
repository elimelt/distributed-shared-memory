#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>

#include "dsm_protocol.h"

#define BUSY_POLL_US 50

dsm_rpc_header_t dsm_rpc_make_header(uint8_t op, uint32_t page_id) {
    dsm_rpc_header_t h;
    memset(&h, 0, sizeof(h));
    h.op = op;
    h.page_id = page_id;
    return h;
}

int dsm_send_full(int fd, const void *buf, size_t len, int flags) {
    const char *ptr = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, MSG_NOSIGNAL | flags);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            return -1;
        }
        ptr += sent;
        remaining -= sent;
    }

    return 0;
}

int dsm_recv_full(int fd, void *buf, size_t len) {
    char *ptr = (char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t received = recv(fd, ptr, remaining, MSG_WAITALL);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (received == 0) {
            return -1;
        }
        ptr += received;
        remaining -= received;
    }

    return 0;
}

int dsm_sendv(int fd, const struct iovec *iov, int iovcnt) {
    struct iovec local_iov[iovcnt];
    memcpy(local_iov, iov, iovcnt * sizeof(struct iovec));

    int remaining_vecs = iovcnt;
    struct iovec *current = local_iov;

    while (remaining_vecs > 0) {
        ssize_t written = writev(fd, current, remaining_vecs);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (written == 0)
            return -1;

        while (written > 0 && remaining_vecs > 0) {
            if ((size_t)written >= current->iov_len) {
                written -= current->iov_len;
                current++;
                remaining_vecs--;
            } else {
                current->iov_base = (char *)current->iov_base + written;
                current->iov_len -= written;
                written = 0;
            }
        }
    }

    return 0;
}

int dsm_setup_socket_optimal(int fd) {
    int flag = 1;
    int bufsize = 1024 * 1024;

    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0)
        return -1;

    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));

#ifdef SO_BUSY_POLL
    int busy_poll = BUSY_POLL_US;
    setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));
#endif

#ifdef SO_INCOMING_CPU
    setsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &flag, sizeof(flag));
#endif

    return 0;
}

int dsm_rpc_get(int fd, uint32_t page_id, void *buf) {
    dsm_rpc_header_t req = dsm_rpc_make_header(OP_GET_PAGE, page_id);

    if (dsm_send_full(fd, &req, sizeof(req), 0) < 0)
        return -1;
    if (dsm_recv_full(fd, buf, PAGE_SIZE) < 0)
        return -1;
    return 0;
}

int dsm_rpc_get_batch(int fd, const uint32_t *ids, int n, void *bufs) {
    if (n <= 0)
        return 0;

    for (int i = 0; i < n; i++) {
        dsm_rpc_header_t req = dsm_rpc_make_header(OP_GET_PAGE, ids[i]);
        int flags = (i < n - 1) ? MSG_MORE : 0;
        if (dsm_send_full(fd, &req, sizeof(req), flags) < 0)
            return -1;
    }

    for (int i = 0; i < n; i++) {
        if (dsm_recv_full(fd, (char *)bufs + (size_t)i * PAGE_SIZE, PAGE_SIZE) < 0)
            return -1;
    }
    return 0;
}

int dsm_rpc_put(int fd, uint32_t page_id, const void *buf) {
    dsm_rpc_header_t req = dsm_rpc_make_header(OP_PUT_PAGE, page_id);

    struct iovec iov[2] = {
        { .iov_base = &req, .iov_len = sizeof(req) },
        { .iov_base = (void *)buf, .iov_len = PAGE_SIZE }
    };
    if (dsm_sendv(fd, iov, 2) < 0)
        return -1;

    dsm_rpc_header_t ack;
    if (dsm_recv_full(fd, &ack, sizeof(ack)) < 0)
        return -1;
    if (ack.op != OP_ACK)
        return -1;
    return 0;
}

