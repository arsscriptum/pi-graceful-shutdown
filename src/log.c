/*
 * log.c - Logging with credential redaction
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <syslog.h>

#include "log.h"

#define LOG_BUF_SIZE 1024
#define SECRET_MAX   64

static char g_secret[SECRET_MAX];
static int  g_secret_len = 0;
static int  g_foreground = 0;
static int  g_verbose    = 0;

/*
 * redact_in_place: replace every occurrence of the secret with asterisks.
 * Inputs: message buffer (modified in place), secret string, secret length.
 * Failure: none; safe on empty secret or empty buffer.
 */
static void redact_in_place(char *buf, const char *secret, int slen)
{
    char *pos;
    int i;

    if (slen <= 0) return;

    pos = buf;
    while ((pos = strstr(pos, secret)) != NULL) {
        for (i = 0; i < slen; i++) {
            pos[i] = '*';
        }
        pos += slen;
    }
}

void log_init(const char *ident, int foreground, int verbose, int level)
{
    g_foreground = foreground;
    g_verbose = verbose;
    openlog(ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    setlogmask(LOG_UPTO(verbose ? LOG_DEBUG : level));
}

void log_set_level(int level)
{
    setlogmask(LOG_UPTO(g_verbose ? LOG_DEBUG : level));
}

void log_set_redact(const char *secret)
{
    if (secret != NULL && secret[0] != '\0') {
        strncpy(g_secret, secret, SECRET_MAX - 1);
        g_secret[SECRET_MAX - 1] = '\0';
        g_secret_len = (int)strlen(g_secret);
    } else {
        g_secret[0] = '\0';
        g_secret_len = 0;
    }
}

void log_close(void)
{
    closelog();
}

void log_msg(int priority, const char *fmt, ...)
{
    char buf[LOG_BUF_SIZE];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (g_secret_len > 0) {
        redact_in_place(buf, g_secret, g_secret_len);
    }

    syslog(priority, "%s", buf);

    if (g_foreground && (g_verbose || priority <= LOG_WARNING)) {
        fprintf(stderr, "%s\n", buf);
    }
}

int log_level_from_string(const char *s)
{
    if (strcmp(s, "error") == 0) return LOG_ERR;
    if (strcmp(s, "warn")  == 0) return LOG_WARNING;
    if (strcmp(s, "info")  == 0) return LOG_INFO;
    if (strcmp(s, "debug") == 0) return LOG_DEBUG;
    return -1;
}
