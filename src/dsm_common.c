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

int dsm_send_full(int fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, MSG_NOSIGNAL);
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

int dsm_recvv(int fd, struct iovec *iov, int iovcnt) {
    struct iovec local_iov[iovcnt];
    memcpy(local_iov, iov, iovcnt * sizeof(struct iovec));

    int remaining_vecs = iovcnt;
    struct iovec *current = local_iov;

    while (remaining_vecs > 0) {
        ssize_t nread = readv(fd, current, remaining_vecs);
        if (nread < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (nread == 0)
            return -1;

        while (nread > 0 && remaining_vecs > 0) {
            if ((size_t)nread >= current->iov_len) {
                nread -= current->iov_len;
                current++;
                remaining_vecs--;
            } else {
                current->iov_base = (char *)current->iov_base + nread;
                current->iov_len -= nread;
                nread = 0;
            }
        }
    }

    return 0;
}

int dsm_set_cork(int fd, int on) {
    int flag = on ? 1 : 0;
    if (setsockopt(fd, IPPROTO_TCP, TCP_CORK, &flag, sizeof(flag)) < 0)
        return -1;
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

int dsm_setup_socket_lowlatency(int fd) {
    int flag = 1;
    int bufsize = 64 * 1024;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag));

#ifdef SO_BUSY_POLL
    int busy_poll = BUSY_POLL_US;
    setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));
#endif

    return 0;
}

int dsm_send_more(int fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, MSG_NOSIGNAL | MSG_MORE);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (sent == 0)
            return -1;
        ptr += sent;
        remaining -= sent;
    }
    return 0;
}

