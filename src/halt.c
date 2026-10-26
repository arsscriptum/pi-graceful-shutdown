/*
 * halt.c - Local halt execution with idempotency latch
 */

#include <stdlib.h>
#include <syslog.h>

#include "halt.h"
#include "log.h"

static int g_latched = 0;

void halt_execute(const char *command, int dry_run)
{
    int rc;

    if (g_latched) {
        log_msg(LOG_INFO, "shutdown already initiated, ignoring signal");
        return;
    }
    g_latched = 1;

    if (dry_run) {
        log_msg(LOG_INFO, "DRY RUN: would execute halt command: %s",
                command);
        return;
    }

    log_msg(LOG_INFO, "executing halt: %s", command);
    rc = system(command);
    if (rc != 0) {
        log_msg(LOG_ERR, "halt command returned %d", rc);
    }
}

int halt_is_latched(void)
{
    return g_latched;
}
