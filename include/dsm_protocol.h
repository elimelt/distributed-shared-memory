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
} dsm_op_type;

/* wire format: 8 bytes total */
typedef struct __attribute__((packed)) {
    uint8_t op;
    uint8_t pad[3];
    uint32_t page_id;
} dsm_rpc_header;

int dsm_send_full(int fd, const void *buf, size_t len);
int dsm_recv_full(int fd, void *buf, size_t len);
int dsm_sendv(int fd, const struct iovec *iov, int iovcnt);
int dsm_recvv(int fd, struct iovec *iov, int iovcnt);
int dsm_set_cork(int fd, int on);
int dsm_setup_socket_optimal(int fd);
int dsm_setup_socket_lowlatency(int fd);
int dsm_send_more(int fd, const void *buf, size_t len);

#endif /* DSM_PROTOCOL_H */

