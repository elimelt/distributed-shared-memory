#ifndef DSM_CONFIG_H
#define DSM_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char *host;
    uint16_t port;
    uint16_t num_pages;
    uint16_t num_virtual_pages;
    uint32_t num_iterations;
    uint8_t locality_percent;
    uint8_t write_percent;
    bool verbose;
    bool benchmark_mode;
    bool verify_mode;
} dsm_client_config_t;

typedef struct {
    uint16_t port;
    uint16_t num_virtual_pages;
    bool verbose;
    char *bind_addr;
} dsm_server_config_t;

dsm_client_config_t dsm_client_config_default(void);
dsm_server_config_t dsm_server_config_default(void);
int dsm_client_config_from_env(dsm_client_config_t *cfg);
int dsm_server_config_from_env(dsm_server_config_t *cfg);
void dsm_client_config_print(const dsm_client_config_t *cfg);
void dsm_server_config_print(const dsm_server_config_t *cfg);

#endif /* DSM_CONFIG_H */

