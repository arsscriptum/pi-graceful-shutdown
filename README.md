# iboot-agent

Graceful-shutdown agent daemon for a Raspberry Pi 5 powered through a
Dataprobe iBoot-G2+ network power switch.

When the iBoot-G2+ is told to turn off or cycle the outlet (from the
web UI, CLI, AutoPing, Schedule, or the front-panel switch), it connects
to this agent over the DxP protocol and signals a shutdown.  The agent
validates the credentials, halts the Pi cleanly, and the iBoot then cuts
AC power after its configured delay.

## Build

Requires only a C compiler and libc (no external dependencies).

```
make
```

The build uses `-ansi -pedantic -Wall -Wextra -O2` with zero warnings.
The target is aarch64 Raspberry Pi OS (Bookworm).  Cross-compilation:

```
make CC=aarch64-linux-gnu-gcc
```

Run the protocol-decode unit tests:

```
make test
```

## Install

```
sudo ./install.sh
```

This places:

- `/usr/local/sbin/iboot-agent` -- the daemon binary
- `/etc/iboot-agent/agent.conf` -- configuration (mode 0600)
- `/etc/systemd/system/iboot-agent.service` -- systemd unit

The service is enabled but not started.  Edit the configuration first.

## Configuration

Edit `/etc/iboot-agent/agent.conf`.  Required settings:

| Key | Description |
|-----|-------------|
| `iboot_address` | IPv4 address or hostname of the iBoot-G2+ |
| `listen_address` | This Pi's LAN IP (bind address for inbound DxP) |
| `shared_username` | Must match the iBoot Graceful Shutdown page |
| `shared_password` | Must match the iBoot Graceful Shutdown page |

Optional settings with defaults:

| Key | Default | Description |
|-----|---------|-------------|
| `iboot_port` | 9100 | DxP port on the iBoot |
| `listen_port` | 9100 | Port the agent listens on |
| `heartbeat_interval_seconds` | 0 (disabled) | KeepAlive interval to iBoot |
| `halt_command` | `/sbin/poweroff` | Command run on shutdown signal |
| `log_level` | `info` | `error`, `warn`, `info`, or `debug` |

The config file must be mode 0600 (readable only by root).  The agent
refuses to start otherwise.

## Usage

### Dry-run testing (recommended first step)

```
sudo iboot-agent --config /etc/iboot-agent/agent.conf --dry-run --verbose
```

Then trigger an Off from the iBoot web UI.  The agent will log the
authenticated shutdown signal without actually halting:

```
DRY RUN: would execute halt command: /sbin/poweroff
```

### Running as a service

```
sudo systemctl start iboot-agent
sudo systemctl status iboot-agent
```

Logs go to the systemd journal:

```
journalctl -u iboot-agent -f
```

### Command-line options

| Flag | Description |
|------|-------------|
| `--config <path>` | Configuration file (default `/etc/iboot-agent/agent.conf`) |
| `--dry-run` | Log signals without halting |
| `--foreground` | Run in foreground (default) |
| `--verbose` | Force debug-level logging |
| `--version` | Print version and exit |
| `--help` | Print usage and exit |

### Signals

| Signal | Action |
|--------|--------|
| `SIGTERM` / `SIGINT` | Clean shutdown of the agent |
| `SIGHUP` | Reload configuration without restart |

## Protocol

The agent implements the server side of Dataprobe's Exchange Protocol
(DxP), documented in `references/dxp_proto_1.2_v101118e.pdf`.

### Connection flow

1. The iBoot connects to the agent on the configured `listen_port`
2. The iBoot sends `hello-000` (10 bytes)
3. The agent responds with a 2-byte little-endian sequence number
4. The iBoot sends a 47-byte DxP header (command, credentials, seq+1)
5. The agent validates the sequence, credentials, and source IP
6. If valid: sends ACK (0x00), executes halt
7. If invalid: sends NAK (0x01) or drops the connection

### Shutdown command detection

The agent treats these DxP commands as shutdown signals:

- `eCmnd_pcControl` (0x07) -- the graceful-shutdown command class
  added after the v1.2 protocol spec
- `eCmnd_io` / `eIO_changeRelay` (0x03/0x01) -- relay-off command

These assumptions must be validated against a packet capture before
production use (see below).

### Heartbeat (optional)

If `heartbeat_interval_seconds` is set, the agent periodically connects
to the iBoot and sends a `eCmnd_keepAlive` command.  This lets the iBoot
know the agent is alive, so it will attempt graceful shutdown rather than
going straight to a hard power cut.

## Protocol-capture validation procedure

Before deploying in production, validate the agent against the actual
DxP traffic from your iBoot unit:

1. Install Wireshark or tcpdump on a machine on the same LAN segment.

2. Run the official Windows iBoot Utility (from Dataprobe) against
   the iBoot.  Configure the Graceful Shutdown page with the shared
   credentials and the Windows machine's IP.

3. Capture traffic on TCP port 9100 during both an Off and a Cycle
   event:
   ```
   sudo tcpdump -i eth0 -w iboot-capture.pcap tcp port 9100
   ```

4. Extract the DxP payloads and identify:
   - Which command class and descriptor the iBoot sends for Off
   - Which command class and descriptor the iBoot sends for Cycle
   - Whether they are distinguishable
   - The heartbeat exchange, if any

5. Encode the captured bytes as test fixtures in `test_dxp.c` (see the
   "Capture-based tests" section) and verify the decoder handles them
   correctly.

6. If the actual command differs from the assumed `eCmnd_pcControl` or
   `eCmnd_io`/`eIO_changeRelay`, update the `is_shutdown_command()`
   function in `iboot-agent.c`.

This step fulfils acceptance criteria AC1 and AC2.

## System dependencies

These settings are required for reliable unattended power cycling but
are outside the agent's responsibility.

### Raspberry Pi 5 EEPROM

Set `POWER_OFF_ON_HALT=1` so the Pi lowers its halt-state current,
allowing supply capacitors to drain quickly after the power cut:

```
sudo rpi-eeprom-config --edit
```

Add or change: `POWER_OFF_ON_HALT=1`

### iBoot timing

On the iBoot web UI, set these delays above the Pi's worst-case halt
time (10-20 seconds).  Recommended values:

- **Hard Shutdown Delay**: 45-60 seconds
- **Hard Reboot Delay**: 45-60 seconds

If the delay is shorter than the Pi's halt time, power is cut while the
filesystem is still being written, risking corruption.

### iBoot Cycle off-interval

For Cycle operations, the off-interval must be long enough for the Pi's
5V rail to fully collapse before power is restored.  Otherwise the Pi
returns in its soft-off latch state and waits for a button press rather
than cold-booting.  A held Off (restored manually later) is inherently
safe because the capacitors fully drain.

## Security considerations

- The DxP protocol carries credentials in cleartext.  The deployment
  assumes a trusted LAN segment.  This is an accepted limitation of the
  protocol, not a defect in this agent.
- The config file is mode 0600 to protect the shared secret.
- The agent validates the source IP of every inbound connection against
  the configured `iboot_address`.
- Credentials in inbound commands are compared against the configured
  shared secret.  Mismatches are rejected and logged.
- The shared password is never written to any log at any level.
- All network input is bounded and validated.  No unbounded reads, no
  format-string sinks, no buffer overruns.

## Least-privilege alternative

The default is to run as root (required for `/sbin/poweroff`).  For a
least-privilege setup:

1. Create a service user: `sudo useradd -r -s /usr/sbin/nologin iboot-agent`
2. Grant it shutdown capability via polkit or sudoers:
   ```
   iboot-agent ALL=(root) NOPASSWD: /sbin/poweroff
   ```
3. Set `halt_command = sudo /sbin/poweroff` in `agent.conf`
4. Change the config file ownership and the systemd unit `User=` field

## License

See LICENSE file (if present) or contact the maintainer.
