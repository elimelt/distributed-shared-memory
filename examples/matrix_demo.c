// demo of high-level dsm_array api for distributed matrix operations

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "dsm_array.h"
#include "dsm_protocol.h"

#define MATRIX_SIZE 64

static int connect_to_server(const char *host, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, host, &addr.sin_addr);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    uint16_t port = 9999;
    
    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);
    
    printf("=== DSM Array API Demo ===\n");
    printf("Connecting to %s:%u...\n", host, port);
    
    size_t total_elements = MATRIX_SIZE * MATRIX_SIZE * 3;
    uint16_t pages_needed = (total_elements * sizeof(double) + PAGE_SIZE - 1) / PAGE_SIZE;
    
    dsm_context_t *ctx = dsm_create_context(128, pages_needed + 128);
    if (!ctx) {
        fprintf(stderr, "Failed to create DSM context\n");
        return 1;
    }
    
    ctx->sock_fd = connect_to_server(host, port);
    if (ctx->sock_fd < 0) {
        fprintf(stderr, "Failed to connect to server\n");
        dsm_destroy_context(ctx);
        return 1;
    }
    
    printf("Connected! Creating %dx%d matrices...\n\n", MATRIX_SIZE, MATRIX_SIZE);

    size_t shape[2] = {MATRIX_SIZE, MATRIX_SIZE};
    
    dsm_array_t *A = dsm_array_create(ctx, DSM_DOUBLE, 2, shape);
    dsm_array_t *B = dsm_array_create(ctx, DSM_DOUBLE, 2, shape);
    dsm_array_t *C = dsm_array_create(ctx, DSM_DOUBLE, 2, shape);
    
    if (!A || !B || !C) {
        fprintf(stderr, "Failed to create arrays\n");
        return 1;
    }
    
    printf("Initializing matrices A and B...\n");

    for (size_t i = 0; i < MATRIX_SIZE; i++) {
        for (size_t j = 0; j < MATRIX_SIZE; j++) {
            dsm_array_set_f64(A, (double)(i + j), i, j);
            dsm_array_set_f64(B, (double)(i * j), i, j);
        }
    }
    
    printf("Computing C = A + B (element-wise)...\n");

    for (size_t i = 0; i < MATRIX_SIZE; i++) {
        for (size_t j = 0; j < MATRIX_SIZE; j++) {
            double a = dsm_array_get_f64(A, i, j);
            double b = dsm_array_get_f64(B, i, j);
            dsm_array_set_f64(C, a + b, i, j);
        }
    }
    
    printf("\nVerification (sample values):\n");
    printf("  A[5,5] = %.1f (expected: 10.0)\n", dsm_array_get_f64(A, 5, 5));
    printf("  B[5,5] = %.1f (expected: 25.0)\n", dsm_array_get_f64(B, 5, 5));
    printf("  C[5,5] = %.1f (expected: 35.0)\n", dsm_array_get_f64(C, 5, 5));
    
    printf("\n=== Statistics ===\n");
    printf("Local hits:     %lu\n", ctx->local_hits);
    printf("Remote fetches: %lu\n", ctx->remote_fetches);
    printf("Evictions:      %lu\n", ctx->evictions);
    printf("Hit rate:       %.1f%%\n", 
           100.0 * ctx->local_hits / (ctx->local_hits + ctx->remote_fetches));

    dsm_array_free(A);
    dsm_array_free(B);
    dsm_array_free(C);
    close(ctx->sock_fd);
    dsm_destroy_context(ctx);
    
    printf("\nDone!\n");
    return 0;
}

