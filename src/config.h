/*
 * config.h - Configuration loading and validation
 *
 * Parses a key=value config file, validates all fields at load time,
 * and enforces 0600 file permissions.  The agent refuses to start
 * on any config error.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <netinet/in.h>

#include "dxp.h"

#define CONFIG_MAX_ADDR 256
#define CONFIG_MAX_CMD  256

struct config {
    char iboot_address[CONFIG_MAX_ADDR];
    int  iboot_port;
    char shared_username[DXP_USERNAME_FIELD];
    char shared_password[DXP_PASSWORD_FIELD];
    int  heartbeat_interval;
    char listen_address[CONFIG_MAX_ADDR];
    int  listen_port;
    char halt_command[CONFIG_MAX_CMD];
    int  log_level;
    struct in_addr iboot_addr_resolved;
};

/*
 * config_set_defaults: fill a config struct with default values.
 * Inputs: pointer to config struct.
 */
void config_set_defaults(struct config *cfg);

/*
 * config_load: parse and validate a config file.
 * Inputs: file path, pointer to config struct (pre-filled with defaults).
 * Returns: 0 on success, -1 on error (message to stderr).
 * Validates: required fields present, ports in range, halt_command
 *            is an absolute path to an existing executable.
 */
int config_load(const char *path, struct config *cfg);

/*
 * config_check_perms: verify the file is not group- or world-readable.
 * Inputs: file path.
 * Returns: 0 if acceptable, -1 otherwise (message to stderr).
 */
int config_check_perms(const char *path);

/*
 * config_resolve_addresses: resolve iboot_address to an in_addr.
 * Inputs: pointer to config struct.
 * Returns: 0 on success, -1 on failure (message to stderr).
 */
int config_resolve_addresses(struct config *cfg);

#endif /* CONFIG_H */
