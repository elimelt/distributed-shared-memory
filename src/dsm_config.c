#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "dsm_config.h"
#include "dsm_protocol.h"
#include "dsm_log.h"

long dsm_env_long(const char *name, long dflt, long min, long max)
{
    const char *val = getenv(name);
    if (!val) {
        return dflt;
    }

    errno = 0;
    char *end = NULL;
    long n = strtol(val, &end, 10);
    if (errno != 0 || end == val || *end != '\0' || n < min || n > max) {
        DSM_LOG_WARN("invalid %s='%s' (expected %ld..%ld); using default %ld",
                     name, val, min, max, dflt);
        return dflt;
    }
    return n;
}

const char *dsm_env_str(const char *name, const char *dflt)
{
    const char *val = getenv(name);
    return val ? val : dflt;
}

static void (*g_usage_fn)(const char *prog);
static const char *g_usage_prog;

void dsm_parse_set_usage(void (*usage)(const char *prog), const char *prog)
{
    g_usage_fn = usage;
    g_usage_prog = prog;
}

long dsm_parse_long(const char *arg, const char *flagname, long min, long max)
{
    errno = 0;
    char *end = NULL;
    long n = strtol(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0' || n < min || n > max) {
        DSM_LOG_ERROR("Invalid value for %s: '%s' (expected %ld..%ld)",
                      flagname, arg, min, max);
        if (g_usage_fn) {
            g_usage_fn(g_usage_prog);
        }
        exit(1);
    }
    return n;
}

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

    cfg->host = (char *)dsm_env_str("DSM_HOST", cfg->host);
    cfg->port = (uint16_t)dsm_env_long("DSM_PORT", cfg->port, 1, 65535);
    cfg->num_pages = (uint32_t)dsm_env_long("DSM_NUM_PAGES",
                                            cfg->num_pages, 1, (long)UINT32_MAX);
    cfg->num_virtual_pages = (uint32_t)dsm_env_long("DSM_NUM_VIRTUAL_PAGES",
                                                    cfg->num_virtual_pages, 1, (long)UINT32_MAX);
    cfg->num_iterations = (uint32_t)dsm_env_long("DSM_NUM_ITERATIONS",
                                                 cfg->num_iterations, 0, (long)UINT32_MAX);
    cfg->locality_percent = (uint8_t)dsm_env_long("DSM_LOCALITY_PERCENT",
                                                  cfg->locality_percent, 0, 100);
    cfg->write_percent = (uint8_t)dsm_env_long("DSM_WRITE_PERCENT",
                                               cfg->write_percent, 0, 100);

    const char *val;

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

    cfg->port = (uint16_t)dsm_env_long("DSM_PORT", cfg->port, 1, 65535);
    cfg->num_virtual_pages = (uint32_t)dsm_env_long("DSM_NUM_VIRTUAL_PAGES",
                                                    cfg->num_virtual_pages, 1, (long)UINT32_MAX);

    const char *val = getenv("DSM_VERBOSE");
    if (val && strcmp(val, "1") == 0) {
        cfg->verbose = true;
    }

    cfg->bind_addr = (char *)dsm_env_str("DSM_BIND_ADDR", cfg->bind_addr);

    return 0;
}

cluster_config_t cluster_config_default(void)
{
    return (cluster_config_t){
        .seed_addr = NULL,
        .seed_port = DEFAULT_PORT + CLUSTER_PORT_OFFSET,
        .node_id = NULL,
        .cluster_port = DEFAULT_PORT + CLUSTER_PORT_OFFSET,
        .heartbeat_ms = HEARTBEAT_INTERVAL_MS,
        .gossip_ms = GOSSIP_INTERVAL_MS,
        .timeout_ms = HEARTBEAT_TIMEOUT_MS,
        .page_range_start = 0,
        .page_range_end = 0
    };
}

int cluster_config_from_env(cluster_config_t *cfg)
{
    if (!cfg) {
        return -1;
    }

    cfg->seed_addr = (char *)dsm_env_str("DSM_SEED_ADDR", cfg->seed_addr);
    cfg->seed_port = (uint16_t)dsm_env_long("DSM_SEED_PORT", cfg->seed_port, 1, 65535);
    cfg->node_id = (char *)dsm_env_str("DSM_NODE_ID", cfg->node_id);
    cfg->cluster_port = (uint16_t)dsm_env_long("DSM_CLUSTER_PORT", cfg->cluster_port, 1, 65535);
    cfg->heartbeat_ms = (uint32_t)dsm_env_long("DSM_HEARTBEAT_MS",
                                               cfg->heartbeat_ms, 1, (long)UINT32_MAX);
    cfg->gossip_ms = (uint32_t)dsm_env_long("DSM_GOSSIP_MS",
                                            cfg->gossip_ms, 1, (long)UINT32_MAX);
    cfg->timeout_ms = (uint32_t)dsm_env_long("DSM_TIMEOUT_MS",
                                             cfg->timeout_ms, 1, (long)UINT32_MAX);
    cfg->page_range_start = (uint32_t)dsm_env_long("DSM_PAGE_RANGE_START",
                                                   cfg->page_range_start, 0, (long)UINT32_MAX);
    cfg->page_range_end = (uint32_t)dsm_env_long("DSM_PAGE_RANGE_END",
                                                 cfg->page_range_end, 0, (long)UINT32_MAX);
    return 0;
}

void dsm_client_config_print(const dsm_client_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    DSM_LOG_INFO("Client Configuration:");
    DSM_LOG_INFO("  Host:              %s", cfg->host ? cfg->host : "(null)");
    DSM_LOG_INFO("  Port:              %u", cfg->port);
    DSM_LOG_INFO("  Num Pages:         %u", cfg->num_pages);
    DSM_LOG_INFO("  Num Virtual Pages: %u", cfg->num_virtual_pages);
    DSM_LOG_INFO("  Num Iterations:    %u", cfg->num_iterations);
    DSM_LOG_INFO("  Locality Percent:  %u%%", cfg->locality_percent);
    DSM_LOG_INFO("  Write Percent:     %u%%", cfg->write_percent);
    DSM_LOG_INFO("  Verbose:           %s", cfg->verbose ? "true" : "false");
    DSM_LOG_INFO("  Verify Mode:       %s", cfg->verify_mode ? "true" : "false");
}

void dsm_server_config_print(const dsm_server_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    DSM_LOG_INFO("Server Configuration:");
    DSM_LOG_INFO("  Port:              %u", cfg->port);
    DSM_LOG_INFO("  Num Virtual Pages: %u", cfg->num_virtual_pages);
    DSM_LOG_INFO("  Verbose:           %s", cfg->verbose ? "true" : "false");
    DSM_LOG_INFO("  Bind Address:      %s", cfg->bind_addr ? cfg->bind_addr : "(null)");
}

void cluster_config_print(const cluster_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    DSM_LOG_INFO("Cluster Configuration:");
    DSM_LOG_INFO("  Seed:          %s:%u", cfg->seed_addr ? cfg->seed_addr : "(none)", cfg->seed_port);
    DSM_LOG_INFO("  Cluster Port:  %u", cfg->cluster_port);
    DSM_LOG_INFO("  Heartbeat:     %u ms", cfg->heartbeat_ms);
    DSM_LOG_INFO("  Gossip:        %u ms", cfg->gossip_ms);
    DSM_LOG_INFO("  Timeout:       %u ms", cfg->timeout_ms);
    DSM_LOG_INFO("  Page Range:    %u - %u", cfg->page_range_start, cfg->page_range_end);
}

