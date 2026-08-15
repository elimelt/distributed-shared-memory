#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsm_config.h"
#include "dsm_protocol.h"

dsm_client_config_t dsm_client_config_default(void)
{
    dsm_client_config_t cfg = {
        .host = DEFAULT_HOST,
        .port = DEFAULT_PORT,
        .num_pages = 256,
        .num_virtual_pages = 1024,
        .num_iterations = 100000,
        .locality_percent = 80,
        .write_percent = 10,
        .verbose = false,
        .verify_mode = false
    };
    return cfg;
}

dsm_server_config_t dsm_server_config_default(void)
{
    dsm_server_config_t cfg = {
        .port = DEFAULT_PORT,
        .num_virtual_pages = 1024,
        .verbose = false,
        .bind_addr = "0.0.0.0"
    };
    return cfg;
}

int dsm_client_config_from_env(dsm_client_config_t *cfg)
{
    if (!cfg) {
        return -1;
    }

    const char *val;

    val = getenv("DSM_HOST");
    if (val) {
        cfg->host = (char *)val;
    }

    val = getenv("DSM_PORT");
    if (val) {
        cfg->port = (uint16_t)atoi(val);
    }

    val = getenv("DSM_NUM_PAGES");
    if (val) {
        cfg->num_pages = (uint32_t)atoi(val);
    }

    val = getenv("DSM_NUM_VIRTUAL_PAGES");
    if (val) {
        cfg->num_virtual_pages = (uint32_t)atoi(val);
    }

    val = getenv("DSM_NUM_ITERATIONS");
    if (val) {
        cfg->num_iterations = (uint32_t)atoi(val);
    }

    val = getenv("DSM_LOCALITY_PERCENT");
    if (val) {
        cfg->locality_percent = (uint8_t)atoi(val);
    }

    val = getenv("DSM_WRITE_PERCENT");
    if (val) {
        cfg->write_percent = (uint8_t)atoi(val);
    }

    val = getenv("DSM_VERBOSE");
    if (val && strcmp(val, "1") == 0) {
        cfg->verbose = true;
    }

    val = getenv("DSM_VERIFY");
    if (val && strcmp(val, "1") == 0) {
        cfg->verify_mode = true;
    }

    return 0;
}

int dsm_server_config_from_env(dsm_server_config_t *cfg)
{
    if (!cfg) {
        return -1;
    }

    const char *val;

    val = getenv("DSM_PORT");
    if (val) {
        cfg->port = (uint16_t)atoi(val);
    }

    val = getenv("DSM_NUM_VIRTUAL_PAGES");
    if (val) {
        cfg->num_virtual_pages = (uint32_t)atoi(val);
    }

    val = getenv("DSM_VERBOSE");
    if (val && strcmp(val, "1") == 0) {
        cfg->verbose = true;
    }

    val = getenv("DSM_BIND_ADDR");
    if (val) {
        cfg->bind_addr = (char *)val;
    }

    return 0;
}

void dsm_client_config_print(const dsm_client_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    printf("Client Configuration:\n");
    printf("  Host:              %s\n", cfg->host ? cfg->host : "(null)");
    printf("  Port:              %u\n", cfg->port);
    printf("  Num Pages:         %u\n", cfg->num_pages);
    printf("  Num Virtual Pages: %u\n", cfg->num_virtual_pages);
    printf("  Num Iterations:    %u\n", cfg->num_iterations);
    printf("  Locality Percent:  %u%%\n", cfg->locality_percent);
    printf("  Write Percent:     %u%%\n", cfg->write_percent);
    printf("  Verbose:           %s\n", cfg->verbose ? "true" : "false");
    printf("  Verify Mode:       %s\n", cfg->verify_mode ? "true" : "false");
}

void dsm_server_config_print(const dsm_server_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    printf("Server Configuration:\n");
    printf("  Port:              %u\n", cfg->port);
    printf("  Num Virtual Pages: %u\n", cfg->num_virtual_pages);
    printf("  Verbose:           %s\n", cfg->verbose ? "true" : "false");
    printf("  Bind Address:      %s\n", cfg->bind_addr ? cfg->bind_addr : "(null)");
}

