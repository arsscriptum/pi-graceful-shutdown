/*
 * halt.h - Local halt execution with idempotency latch
 *
 * Once halt_execute() triggers, the latch is set and all subsequent
 * calls are no-ops.  The agent keeps running until the OS brings it
 * down during the system shutdown, so late duplicate signals never
 * spawn a second halt.
 */

#ifndef HALT_H
#define HALT_H

/*
 * halt_execute: invoke the halt command or log in dry-run mode.
 * Inputs: halt command string, dry_run flag.
 * Output: runs the command (or logs).  Sets the internal latch.
 *         Subsequent calls are logged and ignored.
 * Failure: if the command returns nonzero, logs an error but
 *          keeps the latch set (the iBoot hard-cut is the fallback).
 */
void halt_execute(const char *command, int dry_run);

/*
 * halt_is_latched: check whether halt has already been initiated.
 * Returns: 1 if latched, 0 otherwise.
 */
int halt_is_latched(void);

#endif /* HALT_H */
