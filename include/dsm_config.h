#ifndef DSM_CONFIG_H
#define DSM_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#include "dsm_cluster.h"

typedef struct {
    char *host;
    uint16_t port;
    uint32_t num_pages;
    uint32_t num_virtual_pages;
    uint32_t num_iterations;
    uint8_t locality_percent;
    uint8_t write_percent;
    bool verbose;
    bool verify_mode;
} dsm_client_config_t;

typedef struct {
    uint16_t port;
    uint32_t num_virtual_pages;
    bool verbose;
    char *bind_addr;
} dsm_server_config_t;

dsm_client_config_t dsm_client_config_default(void);
dsm_server_config_t dsm_server_config_default(void);
int dsm_client_config_from_env(dsm_client_config_t *cfg);
int dsm_server_config_from_env(dsm_server_config_t *cfg);
void dsm_client_config_print(const dsm_client_config_t *cfg);
void dsm_server_config_print(const dsm_server_config_t *cfg);

cluster_config_t cluster_config_default(void);
int cluster_config_from_env(cluster_config_t *cfg);
void cluster_config_print(const cluster_config_t *cfg);

/* Checked env parsing: warn to stderr and return dflt on bad input. */
long dsm_env_long(const char *name, long dflt, long min, long max);
const char *dsm_env_str(const char *name, const char *dflt);

/* Checked CLI parsing: print error (and usage, if registered), exit 1 on
 * bad input. */
void dsm_parse_set_usage(void (*usage)(const char *prog), const char *prog);
long dsm_parse_long(const char *arg, const char *flagname, long min, long max);

#endif /* DSM_CONFIG_H */

