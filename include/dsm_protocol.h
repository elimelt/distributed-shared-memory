#ifndef DSM_PROTOCOL_H
#define DSM_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/uio.h>

#define PAGE_SIZE 4096
#define DEFAULT_PORT 9999
#define DEFAULT_HOST "127.0.0.1"
#define MAX_RETRY_ATTEMPTS 10
#define INITIAL_RETRY_DELAY_MS 100

typedef enum {
    OP_GET_PAGE = 1,
    OP_PUT_PAGE = 2,
    OP_ACK = 3
} dsm_op_type_t;

/* wire format: 8 bytes total */
typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t pad[3];
    uint32_t page_id;
} dsm_rpc_header_t;

/* single place that builds fully zero-initialized headers (.pad included) */
dsm_rpc_header_t dsm_rpc_make_header(uint8_t op, uint32_t page_id);

/* monotonic clock in milliseconds (clock_gettime CLOCK_MONOTONIC) */
uint64_t dsm_now_ms(void);

int dsm_send_full(int fd, const void *buf, size_t len, int flags);
int dsm_recv_full(int fd, void *buf, size_t len);
int dsm_sendv(int fd, const struct iovec *iov, int iovcnt);
int dsm_setup_socket_optimal(int fd);

/* client-side RPCs; bufs in dsm_rpc_get_batch is n contiguous pages */
int dsm_rpc_get(int fd, uint32_t page_id, void *buf);
int dsm_rpc_get_batch(int fd, const uint32_t *ids, int n, void *bufs);
int dsm_rpc_put(int fd, uint32_t page_id, const void *buf);

#endif /* DSM_PROTOCOL_H */

