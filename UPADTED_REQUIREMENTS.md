# iboot-agent: Design Document

Architecture and implementation design for the Pi-side graceful-shutdown agent. Companion to `iboot-agent-requirements.md`. The requirements doc states what the program must do; this doc states how it is built. Where they disagree, the requirements doc wins and this doc is corrected.

## 1. System context

Three actors, one network segment:

- iBoot-G2+ (firmware v1.40.60): switches AC to the Pi's USB adapter. Runs DxP on TCP 9100. Hosts two relevant features: Graceful Shutdown (signals a host agent to halt before a cut) and Heartbeat (watchdog that acts on the outlet if host heartbeats stop).
- Raspberry Pi 5: the powered device. Runs this agent.
- The agent: a long-running daemon on the Pi that participates in DxP so the iBoot can halt the Pi cleanly before cutting power, and optionally emits heartbeats so the iBoot's Heartbeat watchdog sees the Pi as alive.

Data path for a clean power-down:
iBoot receives Off or Cycle, with Graceful Shutdown enabled, so it signals the agent over DxP, the agent halts the Pi, the iBoot waits Hard Shutdown/Reboot Delay, then cuts AC.

## 2. Design goals

- Correctness over features. The one job (halt cleanly on a valid signal) must never fail open or fire on noise.
- No dependency beyond libc and systemd. Single static-ish binary.
- Deterministic, inspectable behavior. Every state change logged.
- Safe by construction. A malformed or unauthenticated packet can never cause a halt.

## 3. Architecture overview

Single process, single thread, one event loop over `poll()`. No threads, no forking except optional daemonization. The loop multiplexes:

- the DxP socket (connection to the iBoot, and/or a listening socket, resolved per Section 7),
- a timer for heartbeat emission and reconnect backoff,
- the signal self-pipe for SIGTERM, SIGINT, SIGHUP.

Components, each a translation unit with a narrow interface:

- `config`: load and validate the config file, enforce file permissions.
- `dxp`: DxP framing, credential encoding, frame parse and build. Pure functions over buffers, no I/O. This is the unit tested against the captured reference traffic.
- `session`: owns the socket(s), the connection state machine, reconnect with backoff, heartbeat emission timer.
- `signal_source`: the abstraction that decides a shutdown was commanded (see Section 7). Feeds decoded, authenticated shutdown events to the core.
- `halt`: executes the local power-off, idempotent, latched.
- `log`: syslog wrapper, level filtering, secret redaction.
- `main`: wiring, the poll loop, OS signal handling, lifecycle.

## 4. Process and concurrency model

One thread. Rationale: the workload is a slow heartbeat plus rare events; threads would add race surface to a program whose failure modes power off a host. All blocking is bounded by `poll()` timeouts. No dynamic allocation in the steady loop; fixed buffers sized at startup. Reentrancy from OS signals is handled with the self-pipe pattern so the loop, not the signal handler, does the work.

## 5. Core state machine

States:

- `INIT`: config loaded, logging up, sockets not yet established.
- `CONNECTING`: attempting the DxP session to the iBoot.
- `READY`: session established; emitting heartbeats if enabled; watching for a shutdown signal.
- `SHUTTING_DOWN`: a valid shutdown signal was accepted; halt invoked; latched.
- `STOPPING`: SIGTERM/SIGINT received; clean exit.

Transitions:

- INIT to CONNECTING on startup.
- CONNECTING to READY on successful session establishment.
- CONNECTING to CONNECTING on failure, after backoff.
- READY to CONNECTING on session loss (socket error, missed peer response), after backoff. Reconnect must not require a process restart.
- READY to SHUTTING_DOWN on an authenticated shutdown signal.
- SHUTTING_DOWN is terminal for signal handling: the latch makes further signals log-and-ignore. The process keeps running until the OS takes it down with the system, so late duplicate signals never spawn a second halt.
- any state to STOPPING on SIGTERM/SIGINT.

The latch in SHUTTING_DOWN is the idempotency guarantee (requirements FR6).

## 6. Shutdown execution path

On entry to SHUTTING_DOWN:

1. Set the in-memory latch first, before doing anything else, so a reentrant or duplicate event cannot race a second execution.
2. Log the decision at info, with the signal's provenance (source address, trigger kind) but never the credential.
3. Invoke `halt_command` from config (default `/sbin/poweroff`). Local action is always a clean halt, for both Off and Cycle, per the requirements. Never an OS reboot: the iBoot owns the power, and on a Cycle its power-restore cold-boots the Pi.
4. If the halt command fails to exec, log at error and remain latched. Do not retry in a tight loop; the iBoot's Hard Delay will cut power regardless, which is the designed fallback.

The agent does not attempt to confirm the halt completed; the OS is going down under it. The iBoot's delay is sized to exceed worst-case halt time (external dependency, Section 11).

## 7. DxP protocol layer and the signal-source abstraction

This is the one area with unresolved specifics. The reference material (`/references`: DxP Protocol Documentation, DxP Programming Examples, iBoot Control Utility C code) plus a packet capture of the official Windows utility against this iBoot are the ground truth. The C control-utility source is the controller direction (host to iBoot) and gives DxP connection setup, framing, and credential encoding; it does not contain the graceful-shutdown receive path, which must come from the protocol doc and the capture.

Two candidate mechanisms for how the iBoot tells the agent to shut down. The design does not commit until the capture decides:

- Mechanism A, heartbeat reply flag: the agent sends periodic DxP heartbeats to the iBoot; the iBoot's reply carries a shutdown-pending flag. The `signal_source` is then a reply watcher inside `session`.
- Mechanism B, inbound command: the iBoot opens a connection to the agent and pushes a shutdown command; the agent listens. The `signal_source` is then a listener bound to a specific address.

`signal_source` is an interface (emit an authenticated `shutdown_event` to the core) with two implementations, A and B. Only one is compiled in active use once the capture resolves which the firmware does. Building the abstraction first keeps the core and the state machine independent of that answer.

Authentication is enforced in the `dxp` unit, not in `session`: every candidate shutdown event carries the shared credential and is validated before it is allowed to become a `shutdown_event`. As defense in depth, `session` rejects frames whose source address is not the configured iBoot. A frame that fails either check is dropped and logged as a validation failure; it never reaches the core.

Open items to resolve from the capture, carried from the requirements doc: exact port and whether it is fixed at 9100 on this firmware; frame layout and credential encoding; connection direction (A vs B); expected heartbeat interval and the iBoot's missed-beat timeout; whether the iBoot associates the agent by source IP; whether Off and Cycle are distinguishable (log the distinction if present, act identically either way).

## 8. Heartbeat emission (optional, config-gated)

If the iBoot's Heartbeat watchdog is enabled with Source = Network on 9100, the agent emits heartbeats at the configured interval so the Pi reads as alive. This is separate from the graceful-shutdown path and is gated by `heartbeat_interval_seconds` being set.

Interaction hazard to handle in design: when the agent halts on a graceful signal, it stops heartbeating. A Heartbeat watchdog could read that as a failure and trigger its own Cycle on top of the intended Off. The design mitigation is documentation and configuration, not code: the Heartbeat Fail Count times the interval must exceed the Hard Shutdown Delay window, so the intended cut happens before the watchdog would act. The agent logs at info when it stops heartbeating due to a shutdown, so the sequence is traceable.

## 9. Configuration design

Parser is a minimal `key = value` reader (requirements Section 9), comments on `#`, whitespace trimmed, unknown keys rejected with a clear error. Values validated at load: address parses, port in range, interval positive, `halt_command` is an absolute path to an existing executable. The agent refuses to start if the config file is group- or world-readable, because it holds the shared secret. Config is loaded fully before any socket opens, so a bad config fails fast and loud, never half-configured.

## 10. Logging and observability

All logging through the `log` unit to syslog, with an optional stderr mirror in `--foreground`. Levels per `log_level`. Hard rule enforced at the wrapper: the credential value is never passed to a log call; redaction is the wrapper's job, not the caller's discipline. Logged events: state transitions, session connect and loss, each heartbeat milestone at debug, every received candidate event with its validation result, the shutdown decision, and halt exec success or failure.

## 11. Error handling and recovery

- Session loss: return to CONNECTING, reconnect with capped exponential backoff. No restart required.
- iBoot rebooted or network blip: same path, transparently recovered.
- Halt exec failure: logged, latch held, rely on the iBoot hard-cut fallback.
- Config error at SIGHUP reload: keep running on the old config, log the error, do not drop the session.
- Unknown or malformed frame: dropped, counted, logged. Never fatal.

Fallback philosophy: if the agent is dead or wrong, the iBoot still cuts power after its delay. The agent improves the shutdown from hard to clean; it is never the thing that keeps power on.

## 12. Security design

- Shared secret only in the 0600 root-owned config and in memory; never logged, never on the command line.
- Source-address check on inbound frames as defense in depth.
- If Mechanism B (listener) is used, bind to the configured interface address, never 0.0.0.0.
- DxP is cleartext (old protocol, no transport security). Accepted limitation; deployment assumes a trusted segment. Documented, not worked around.
- Every network-sourced field is bounds-checked before use. This daemon can power off the host, so input validation is treated as a safety property, not a nicety.

## 13. Lifecycle and systemd

`Type=simple`, foreground, `Restart=on-failure`, `RestartSec=5`, `After=network-online.target`, `Wants=network-online.target`, `WantedBy=multi-user.target`. Runs as root for poweroff rights (least-privilege alternative documented in the README). SIGTERM and SIGINT close sockets and exit 0. SIGHUP reloads config per Section 11.

## 14. Source layout

```
iboot-agent/
  src/
    main.c
    config.c      config.h
    dxp.c         dxp.h        (pure, unit-tested against the capture)
    session.c     session.h
    signal_source.c signal_source.h   (A and B implementations)
    halt.c        halt.h
    log.c         log.h
  test/
    dxp_decode_test.c
    fixtures/graceful_off.pcap
    fixtures/graceful_cycle.pcap
  Makefile
  agent.conf.example
  iboot-agent.service
  install.sh
  README.md
```

Coding standard per the requirements doc: ISO C90 core plus POSIX.1, all locals declared at the top of their function ordered largest to smallest, clean under `-ansi -pedantic -Wall -Wextra`, and the em dash character never appears anywhere including comments.

## 15. External dependencies (not implemented by this program)

- Pi 5 EEPROM `POWER_OFF_ON_HALT=1`, so the post-cut cold boot is reliable.
- iBoot Hard Shutdown/Reboot Delay above worst-case Pi halt time (45 to 60s recommended; Pi 5 halts in roughly 10 to 20s).
- iBoot Cycle off-interval long enough to collapse the 5V rail, or the Pi returns latched and waits for the button.

These govern the power-on side, which the agent cannot touch. They belong in the README.

## 16. Related hardware channel (out of scope, noted)

The iBoot EXP ports provide a GPIO side channel (V+ 12V, I input, G, O open-collector output). It is not part of this program and not a viable shutdown-warning path: the EXP output reflects outlet state and only changes at the moment of the cut, too late to warn the Pi. The EXP input and DxP Triggers drive remote devices, unrelated to this Pi's own shutdown. If a future revision wants a hardware confirmation of outlet state, the Pi could read O on a GPIO with a 3.3V pull-up and shared ground, with the input driven only through isolation. That is a separate design, deliberately excluded here.

## 17. Open design questions

1. Mechanism A vs B. Blocks finalizing `signal_source`. Resolve from the capture first.
2. Credential encoding in DxP frames. Needed for both auth validation and, if A, heartbeat construction.
3. Heartbeat interval and the iBoot missed-beat timeout, to set safe defaults and the Section 8 interaction margin.
4. Whether this firmware fixes the DxP port at 9100 or honors the configurable DxP Port field.

Resolve 1 through 4 by capturing the Windows utility against this exact iBoot during an Off and a Cycle, before writing `session` and `signal_source`. `dxp` and its decode test can be built against the capture independently of that decision.
