/*
 * DSM Server with io_uring support
 * High-performance async I/O version
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "dsm_protocol.h"
#include "dsm_config.h"
#include "dsm_uring.h"

static uring_server_t *g_uring_srv = NULL;

static void handle_signal(int sig) {
    (void)sig;
    if (g_uring_srv) uring_server_stop(g_uring_srv);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("io_uring-based DSM Server\n\n");
    printf("Options:\n");
    printf("  -p, --port PORT          Listen port (default: 9999)\n");
    printf("  -b, --bind ADDR          Bind address (default: 0.0.0.0)\n");
    printf("  -n, --pages NUM          Number of pages (default: 1024)\n");
    printf("  -v, --verbose            Verbose output\n");
    printf("  -h, --help               Show this message\n");
}

static int start_server(uint16_t port, const char *bind_addr) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, bind_addr, &addr.sin_addr);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 128) < 0) {
        perror("listen"); close(fd); return -1;
    }

    printf("[io_uring] DSM Server listening on %s:%d\n", bind_addr, port);
    return fd;
}

int main(int argc, char **argv) {
    dsm_server_config_t cfg = dsm_server_config_default();
    dsm_server_config_from_env(&cfg);

    static struct option long_opts[] = {
        {"port", required_argument, 0, 'p'},
        {"bind", required_argument, 0, 'b'},
        {"pages", required_argument, 0, 'n'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:b:n:vh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p': cfg.port = (uint16_t)atoi(optarg); break;
        case 'b': cfg.bind_addr = optarg; break;
        case 'n': cfg.num_virtual_pages = (uint16_t)atoi(optarg); break;
        case 'v': cfg.verbose = true; break;
        case 'h': print_usage(argv[0]); return 0;
        default: print_usage(argv[0]); return 1;
        }
    }

    struct sigaction sa = { .sa_handler = handle_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Allocate page storage */
    size_t storage_size = (size_t)cfg.num_virtual_pages * PAGE_SIZE;
    uint8_t *storage = mmap(NULL, storage_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (storage == MAP_FAILED) { perror("mmap"); return 1; }

    /* Storage is zero-initialized by mmap (MAP_ANONYMOUS) - no memset needed */
    /* Use madvise to hint that we'll access this memory soon */
    madvise(storage, storage_size, MADV_WILLNEED);

    int listen_fd = start_server(cfg.port, cfg.bind_addr);
    if (listen_fd < 0) return 1;

    /* Create io_uring server */
    uring_server_t srv;
    if (uring_server_init(&srv, listen_fd, storage, cfg.num_virtual_pages) < 0) {
        fprintf(stderr, "Failed to init io_uring\n");
        close(listen_fd);
        munmap(storage, storage_size);
        return 1;
    }
    g_uring_srv = &srv;

    if (cfg.verbose) dsm_server_config_print(&cfg);

    /* Run event loop */
    uring_server_run(&srv);

    /* Cleanup */
    uring_server_destroy(&srv);
    close(listen_fd);
    munmap(storage, storage_size);
    printf("Server shutdown\n");
    return 0;
}

