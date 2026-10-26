/*
 * log.h - Logging interface with credential redaction
 *
 * All logging passes through this module.  The wrapper scans every
 * formatted message for the shared secret and replaces it with
 * asterisks before the message reaches syslog or stderr.  Callers
 * should still avoid passing the credential, but the wrapper
 * enforces the policy as a safety net.
 */

#ifndef LOG_H
#define LOG_H

/*
 * log_init: open syslog, set initial options.
 * Inputs: syslog ident string, whether to mirror to stderr,
 *         whether verbose (forces debug level), syslog priority ceiling.
 */
void log_init(const char *ident, int foreground, int verbose, int level);

/*
 * log_set_level: change the active log level.
 * Inputs: syslog priority (LOG_ERR, LOG_WARNING, LOG_INFO, LOG_DEBUG).
 */
void log_set_level(int level);

/*
 * log_set_redact: register the secret string to scrub from log output.
 * Inputs: the shared password (copied internally).
 * Call once after loading config, and again after a config reload.
 */
void log_set_redact(const char *secret);

/*
 * log_close: close syslog.
 */
void log_close(void);

/*
 * log_msg: format and emit a log message.
 * Inputs: syslog priority, printf-style format and arguments.
 * Output: message to syslog; also to stderr if foreground mode
 *         and priority <= LOG_WARNING or verbose is set.
 * The secret (if registered) is scrubbed before output.
 */
void log_msg(int priority, const char *fmt, ...);

/*
 * log_level_from_string: convert a level name to syslog priority.
 * Inputs: level name ("error", "warn", "info", "debug").
 * Returns: syslog priority, or -1 on unrecognized input.
 */
int log_level_from_string(const char *s);

#endif /* LOG_H */
