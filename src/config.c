/*
 * config.c - Configuration loading and validation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netdb.h>
#include <syslog.h>

#include "config.h"
#include "log.h"

#define MAX_LINE 512

/*
 * trim: strip leading and trailing whitespace in place.
 * Inputs: null-terminated string.
 * Returns: pointer to first non-whitespace character.
 */
static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (*s == '\0') return s;

    end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' ||
                       *end == '\n' || *end == '\r'))
        end--;
    *(end + 1) = '\0';
    return s;
}

void config_set_defaults(struct config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->iboot_port = DXP_DEFAULT_PORT;
    cfg->listen_port = DXP_DEFAULT_PORT;
    cfg->heartbeat_interval = 0;
    strncpy(cfg->halt_command, "/sbin/poweroff", CONFIG_MAX_CMD - 1);
    cfg->log_level = LOG_INFO;
}

int config_load(const char *path, struct config *cfg)
{
    FILE *fp;
    char line[MAX_LINE];
    char *p;
    char *eq;
    char *key;
    char *val;
    struct stat st;
    int lineno;

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "cannot open config %s: %s\n", path, strerror(errno));
        return -1;
    }

    lineno = 0;
    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        lineno++;

        p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        eq = strchr(p, '=');
        if (eq == NULL) {
            fprintf(stderr, "%s:%d: missing '=' in line\n", path, lineno);
            fclose(fp);
            return -1;
        }

        *eq = '\0';
        key = trim(p);
        val = trim(eq + 1);

        if (strcmp(key, "iboot_address") == 0) {
            strncpy(cfg->iboot_address, val, CONFIG_MAX_ADDR - 1);
        } else if (strcmp(key, "iboot_port") == 0) {
            cfg->iboot_port = atoi(val);
        } else if (strcmp(key, "shared_username") == 0) {
            if (strlen(val) > DXP_USERNAME_MAX) {
                fprintf(stderr, "%s:%d: username exceeds %d characters\n",
                        path, lineno, DXP_USERNAME_MAX);
                fclose(fp);
                return -1;
            }
            strncpy(cfg->shared_username, val, DXP_USERNAME_FIELD - 1);
        } else if (strcmp(key, "shared_password") == 0) {
            if (strlen(val) > DXP_PASSWORD_MAX) {
                fprintf(stderr, "%s:%d: password exceeds %d characters\n",
                        path, lineno, DXP_PASSWORD_MAX);
                fclose(fp);
                return -1;
            }
            strncpy(cfg->shared_password, val, DXP_PASSWORD_FIELD - 1);
        } else if (strcmp(key, "heartbeat_interval_seconds") == 0) {
            cfg->heartbeat_interval = atoi(val);
        } else if (strcmp(key, "listen_address") == 0) {
            strncpy(cfg->listen_address, val, CONFIG_MAX_ADDR - 1);
        } else if (strcmp(key, "listen_port") == 0) {
            cfg->listen_port = atoi(val);
        } else if (strcmp(key, "halt_command") == 0) {
            strncpy(cfg->halt_command, val, CONFIG_MAX_CMD - 1);
        } else if (strcmp(key, "log_level") == 0) {
            cfg->log_level = log_level_from_string(val);
            if (cfg->log_level < 0) {
                fprintf(stderr, "%s:%d: unknown log level '%s'\n",
                        path, lineno, val);
                fclose(fp);
                return -1;
            }
        } else {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, lineno, key);
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);

    /* Validate required fields */
    if (cfg->iboot_address[0] == '\0') {
        fprintf(stderr, "%s: iboot_address is required\n", path);
        return -1;
    }
    if (cfg->shared_username[0] == '\0') {
        fprintf(stderr, "%s: shared_username is required\n", path);
        return -1;
    }
    if (cfg->shared_password[0] == '\0') {
        fprintf(stderr, "%s: shared_password is required\n", path);
        return -1;
    }
    if (cfg->listen_address[0] == '\0') {
        fprintf(stderr, "%s: listen_address is required\n", path);
        return -1;
    }
    if (cfg->iboot_port <= 0 || cfg->iboot_port > 65535) {
        fprintf(stderr, "%s: iboot_port out of range\n", path);
        return -1;
    }
    if (cfg->listen_port <= 0 || cfg->listen_port > 65535) {
        fprintf(stderr, "%s: listen_port out of range\n", path);
        return -1;
    }

    /* Validate halt_command is an absolute path */
    if (cfg->halt_command[0] != '/') {
        fprintf(stderr, "%s: halt_command must be an absolute path\n", path);
        return -1;
    }
    /* Check the executable exists (first word of the command) */
    {
        char exe[CONFIG_MAX_CMD];
        char *sp;

        strncpy(exe, cfg->halt_command, CONFIG_MAX_CMD - 1);
        exe[CONFIG_MAX_CMD - 1] = '\0';
        sp = strchr(exe, ' ');
        if (sp != NULL) *sp = '\0';

        if (stat(exe, &st) < 0) {
            fprintf(stderr, "%s: halt_command executable '%s': %s\n",
                    path, exe, strerror(errno));
            return -1;
        }
        if (!(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            fprintf(stderr, "%s: halt_command '%s' is not executable\n",
                    path, exe);
            return -1;
        }
    }

    return 0;
}

int config_check_perms(const char *path)
{
    struct stat st;

    if (stat(path, &st) < 0) {
        fprintf(stderr, "cannot stat %s: %s\n", path, strerror(errno));
        return -1;
    }
    if ((st.st_mode & (S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)) != 0) {
        fprintf(stderr,
                "%s: config file must not be group- or world-readable "
                "(mode 0600 required); current mode %04o\n",
                path, (unsigned)(st.st_mode & 0777));
        return -1;
    }
    return 0;
}

int config_resolve_addresses(struct config *cfg)
{
    struct addrinfo hints;
    struct addrinfo *res;
    struct sockaddr_in *sa;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(cfg->iboot_address, NULL, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "cannot resolve '%s': %s\n",
                cfg->iboot_address, gai_strerror(rc));
        return -1;
    }

    sa = (struct sockaddr_in *)res->ai_addr;
    cfg->iboot_addr_resolved = sa->sin_addr;
    freeaddrinfo(res);
    return 0;
}
