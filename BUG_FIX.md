# BUG_FIX: relay-state misclassification in the shutdown decoder

Date: 2026-10-31
Branch: `new-design-v2`
Origin: `patch/iboot-agent-relay-state-fix.patch` (ported, see
"Notes on the supplied patch" below)

## 1. Summary

The agent decided whether an inbound DxP frame was a shutdown signal
from the 47-byte header alone. For `eCmnd_io` (3) / `eIO_changeRelay`
(1) that is not enough information: the header says the outlet relay
is being changed, but not *what* it is being changed to. The relay
state lives in the 2-byte payload that follows the header.

Result: a `changeRelay` carrying `ENERGIZE` (the outlet being switched
**on**) was decoded as a shutdown signal and halted the Pi. An
outlet-on command from the iBoot therefore powered the Pi down instead
of leaving it running.

## 2. The bug

### Where

`src/dxp.c`, `dxp_is_shutdown_command()`, called from
`src/signal_source.c`, `signal_source_handle_connection()`.

### Pre-fix code

```c
int dxp_is_shutdown_command(const dxp_header_t *hdr)
{
    if (hdr->command == DXP_CMND_PCCONTROL) {
        return 1;
    }
    if (hdr->command == DXP_CMND_IO &&
        hdr->desc == DXP_IO_CHANGE_RELAY) {
        return 1;          /* <-- state never consulted */
    }
    return 0;
}
```

The function takes only `hdr`. The relay state byte is not in the
header, so there was no way for it to be consulted. Every
`changeRelay` was a shutdown, whichever of the three states it
carried:

| DxP relay state | Meaning | Correct action | Pre-fix action |
|---|---|---|---|
| `DXP_RELAY_NO_CHANGE` (0) | leave outlet as is | ignore | **halt** |
| `DXP_RELAY_ENERGIZE` (1) | outlet switching on | ignore | **halt** |
| `DXP_RELAY_RELAX` (2) | outlet switching off | halt | halt |

### Observable effect

With `POWER_OFF_ON_HALT=1` set in the Pi 5 bootloader EEPROM (required
by Section 13 of CLAUDE.md), a halt puts the Pi into soft-off. Nothing
on the iBoot side then brings it back, because the iBoot had just
commanded power **on**, not a cycle. The Pi sat in soft-off waiting for
the physical button, which is exactly the unattended-recovery failure
the project exists to prevent. In effect an outlet-on command reset the
Pi from a distance and left it down.

### Severity

High. This daemon's only action is to power off the host, so a
misclassification is an unplanned outage, and one that cannot be
recovered remotely.

### Why it was not caught by the tests

`test_is_shutdown_command()` asserted the buggy behaviour as if it were
the specification:

```c
/* io/changeRelay (command 3, desc 1) is a shutdown */
hdr.command = DXP_CMND_IO;
hdr.desc = DXP_IO_CHANGE_RELAY;
ASSERT_EQ_INT("io/changeRelay", 1, dxp_is_shutdown_command(&hdr));
```

The relay state was absent from the test as well, so the suite was
green (92/92) with the defect in place.

The reference capture in `references/results.txt` did not expose it
either: all three captured frames are `eCmnd_pcControl` (command 7,
desc 2), header-only, 69 bytes total (22-byte JSON envelope prefix plus
47-byte header, no payload). The `pcControl` path was always correct.
The bug sat on the `changeRelay` path, which that capture does not
cover.

## 3. The fix

`dxp_is_shutdown_command(hdr)` is replaced by

```c
int dxp_classify_shutdown(const dxp_header_t *hdr,
                          const unsigned char *payload,
                          int payload_len);
```

which returns one of three verdicts:

| Return | Meaning |
|---|---|
| `DXP_SHUTDOWN_YES` (1) | halt the host |
| `DXP_SHUTDOWN_NO` (0) | ignore the frame |
| `DXP_SHUTDOWN_NO_STATE` (-1) | `changeRelay` arrived with no readable state payload; do not halt, but log it |

Rules:

- `eCmnd_pcControl` (7): shutdown, any descriptor, payload or not.
  This is the documented graceful-shutdown command and the one the
  reference capture shows, and it is header-only.
- `eCmnd_io` (3) / `eIO_changeRelay` (1): shutdown **only** when
  `payload[1] == DXP_RELAY_RELAX`. `ENERGIZE` and `NO_CHANGE` are
  ignored, and so is any unrecognised state byte.
- A `changeRelay` with a truncated or absent payload returns
  `NO_STATE` and does not halt. Refusing to act beats guessing, and it
  satisfies the Section 10 rule that a malformed packet must never do
  more than get rejected.
- Everything else is ignored, as before.

The caller locates the payload from the frame it has already buffered.
`dxp_parse_json_envelope()` gained an optional `int *hdr_off` output so
`signal_source.c` knows where the embedded header starts and therefore
where the payload begins:

```c
payload     = raw + hdr_off + DXP_HDR_SIZE;
payload_len = raw_len - hdr_off - DXP_HDR_SIZE;
```

No extra `recv()` is issued. The whole frame is already drained into
`raw[]` by `recv_accumulate()` before parsing, so the payload is in
hand. (The supplied patch did a second `recv_exact()` at this point,
which would not work in this code base; see below.)

### Logging

The relay index and state are now logged on both branches, so a field
diagnosis no longer needs a packet capture:

```
connection: changeRelay relay=0 state=RELAX
connection: changeRelay relay=0 state=1 (not RELAX, ignoring)
connection: changeRelay without a readable state payload (0 byte(s)); not treating as shutdown
```

The JSON-envelope debug line now also reports the header offset.
No credential material is logged, at any level (AC8 unchanged).

## 4. Changelist

| File | Change |
|---|---|
| `src/dxp.h` | Added `DXP_CHANGERELAY_PAYLOAD_LEN`, `DXP_CR_OFF_RELAY`, `DXP_CR_OFF_STATE` documenting the 2-byte changeRelay payload. Added the `DXP_SHUTDOWN_NO` / `_YES` / `_NO_STATE` verdict codes. Replaced the `dxp_is_shutdown_command()` declaration with `dxp_classify_shutdown()`. Added the `hdr_off` output parameter to `dxp_parse_json_envelope()`. |
| `src/dxp.c` | Implemented `dxp_classify_shutdown()` with the payload-aware rules. `dxp_parse_json_envelope()` now reports the embedded header offset. |
| `src/signal_source.c` | Derives the payload slice from the buffered frame and the header offset, calls the new classifier, handles the three verdicts, and logs relay index and state. |
| `test/dxp_decode_test.c` | `test_is_shutdown_command()` rewritten as `test_classify_shutdown()`, covering RELAX, ENERGIZE, NO_CHANGE, an unknown state byte, a non-zero relay index, an absent payload, a 1-byte payload, and an over-long payload. Added `test_envelope_changerelay()`: a synthetic JSON-envelope changeRelay frame built on the real capture prefix, decoded end to end. The capture test now also asserts the header offset (22) and that the captured frame carries no payload. Envelope call sites updated for the new signature, including a NULL-offset call. |
| `README.md` | Corrected the stale pointer to `is_shutdown_command()` in `iboot-agent.c` (the file no longer exists) to `dxp_classify_shutdown()` in `src/dxp.c`, and recorded the relay-state rule. Fixed the stale `test_dxp.c` path. |

No change to the configuration format, the systemd unit, the install
paths, or the command-line interface.

## 5. Validation

### Reproduction, before the fix

Pre-fix sources from `HEAD`, built and driven with a `changeRelay`
`ENERGIZE` frame on loopback:

```
connection: shutdown signal received (command=io desc=1)
initiating halt (source=127.0.0.1 command=io(3) desc=1)
DRY RUN: would execute halt command: /bin/true
```

An outlet-on command halts the host. Confirmed.

### Unit tests

```
$ make test
110 passed, 0 failed, 110 total
```

Was 92/92 before; the 18 added assertions cover the relay states and
the envelope changeRelay decode path.

### Build

Clean under the required flags, zero warnings (AC9):

```
cc -ansi -pedantic -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200112L -D_DEFAULT_SOURCE
```

### End-to-end, after the fix

A fake iBoot on loopback performs the real exchange (hello, sequence,
JSON-enveloped frame) against the daemon in `--dry-run`, one fresh
process per case so the FR6 latch does not mask a result:

| Case | Frame | Expected | Result |
|---|---|---|---|
| `energize` | io/changeRelay, state 1 | no halt | pass |
| `nochange` | io/changeRelay, state 0 | no halt | pass |
| `nopayload` | io/changeRelay, header only | no halt | pass |
| `badcred` | io/changeRelay RELAX, wrong password | no halt | pass |
| `getrelays` | io/getRelays | no halt | pass |
| `relax` | io/changeRelay, state 2 | halt | pass |
| `pccontrol` | pcControl desc=2 (capture shape) | halt | pass |

### Memory safety (AC10)

valgrind 3.26.0, `--leak-check=full --show-leak-kinds=all
--track-origins=yes --error-exitcode=9`.

Test binary:

```
total heap usage: 1 allocs, 1 frees, 4,096 bytes allocated
All heap blocks were freed -- no leaks are possible
ERROR SUMMARY: 0 errors from 0 contexts
```

Daemon, each of the seven end-to-end cases above run as its own
valgrind'd process: 0 errors, 0 leaks, exit 0 in all seven. Both the
halt path and the ignore path are covered.

Soak, one valgrind'd daemon process handling **64 connections** without
a restart:

- 50 valid-credential exchanges across the five non-halting cases
  (10 rounds of energize, nochange, nopayload, badcred, getrelays)
- 9 malformed or hostile frames: empty, short random, 47 random bytes,
  300 random bytes, all zeros, 1200 bytes (over the `raw[]` bound),
  brace with a bad tag, a truncated envelope header, and a bad hello
- two `SIGHUP` config reloads mid-run, with more traffic after them
- a `RELAX` frame (halted, as it should), then a second `RELAX`
  (suppressed by the FR6 latch)
- `SIGTERM`, clean exit 0

Result:

```
total heap usage: 14 allocs, 14 frees, 18,516 bytes allocated
All heap blocks were freed -- no leaks are possible
ERROR SUMMARY: 0 errors from 0 contexts
```

14 allocations for 64 connections, so the steady-state loop allocates
nothing per connection, as CLAUDE.md Section 6 requires. Tallies from
that run:

| Outcome | Count |
|---|---|
| ENERGIZE or NO_CHANGE ignored | 22 (11 of each) |
| changeRelay with no state payload | 10 |
| credential rejections | 15 |
| other non-shutdown commands ignored | 11 |
| halts performed | 1 |
| latch suppressions (FR6) | 1 |
| config reloads (SIGHUP) | 4 log lines, 2 signals |
| **occurrences of the shared password in the log (AC8)** | **0** |

Every malformed frame was rejected and logged (`invalid hello message`,
`no data after hello/seq`, `unrecognized frame (8 bytes)`,
`unrecognized frame (23 bytes)`), and none halted the host (AC5).

AC10 also calls for a 24 hour run on the target; this soak is the same
code paths at much higher event density, but the duration test still
belongs on the Pi.

### Not covered

AC3, AC4, AC6 and AC7 need the real iBoot-G2+ and the Pi; they were not
re-run as part of this fix, nor was AC10's 24 hour duration. The `changeRelay` payload layout itself is
taken from the DxP protocol documentation and the patch, and is not yet
confirmed by a capture of a real `changeRelay` frame, because the
capture in `references/results.txt` only contains `pcControl`. The
`pcControl` path is what the unit actually uses today, and it is
covered by the committed capture fixture.

## 6. Notes on the supplied patch

`patch/iboot-agent-relay-state-fix.patch` could not be applied as
given. It is recorded here so the next person does not retry it:

1. **Wrong layout.** The patch targets a flat single-file tree
   (`./dxp.h`, `./iboot-agent.c`, `./test_dxp.c`). This branch is the
   split `src/` plus `test/` layout, and no branch in this repository
   has those root-level files:

   ```
   $ git apply --check patch/iboot-agent-relay-state-fix.patch
   error: dxp.h: No such file or directory
   error: iboot-agent.c: No such file or directory
   error: test_dxp.c: No such file or directory
   ```

2. **Pre-dates the JSON envelope.** The copies in `patch/` have no
   `dxp_parse_json_envelope()`, so they cannot decode what firmware
   v1.40.60 actually sends. Copying them over `src/` would have
   reverted that support.

3. **Its read of the payload would not work here.** The patch calls
   `recv_exact(fd, cr_payload, 2)` after validating the header. In
   this code base `recv_accumulate()` has already drained the socket
   into `raw[]`, so that call would find nothing, block for the
   receive timeout, and then log "changeRelay without a readable
   state payload" for every real changeRelay. The payload is read
   from the buffer instead.

4. **`patch/apply.sh` is broken** independently of the above:
   `cp "./patch/*" "$ROOT_PATH" -v` quotes the glob so it is passed
   literally and the copy fails; it then runs `git apply` on a path
   relative to the wrong directory; and it would overwrite the current
   sources with the pre-envelope copies before applying the patch on
   top.

The fix itself is correct and was adopted. What was ported is the
intent (consult the relay state; `ENERGIZE` must never halt) and the
payload constants, re-expressed for this layout and routed through the
pure, unit-tested `dxp.c` protocol layer so the rule is covered by
AC1 rather than buried in connection handling. `patch/` is left
untouched as the historical record.
