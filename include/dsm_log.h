#ifndef DSM_LOG_H
#define DSM_LOG_H

#include <stdio.h>

/* Nonzero enables DSM_LOG_DEBUG output. Defined in dsm_common.c; each main
 * sets it from its config's `verbose` field after parsing. */
extern int dsm_log_verbose;

/* Logging macros append the newline; call sites pass the bare message.
 * ERROR/WARN always go to stderr, INFO to stdout, DEBUG to stdout only
 * when dsm_log_verbose is nonzero. */
#define DSM_LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define DSM_LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define DSM_LOG_INFO(fmt, ...)  printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define DSM_LOG_DEBUG(fmt, ...) \
    do { \
        if (dsm_log_verbose) \
            printf("[DEBUG] " fmt "\n", ##__VA_ARGS__); \
    } while (0)

#endif /* DSM_LOG_H */
