#!/bin/sh
#
# install.sh - Install iboot-agent on a Raspberry Pi 5
#
# Must be run as root.  Installs the binary, configuration,
# and systemd unit, then enables (but does not start) the service.

set -e

BINDIR="/usr/local/sbin"
CONFDIR="/etc/iboot-agent"
UNITDIR="/etc/systemd/system"
BINARY="iboot-agent"
UNIT="iboot-agent.service"
CONF="agent.conf"
CONF_EXAMPLE="agent.conf.example"

if [ "$(id -u)" -ne 0 ]; then
    echo "error: must be run as root" >&2
    exit 1
fi

if [ ! -f "$BINARY" ]; then
    echo "error: $BINARY not found; run 'make' first" >&2
    exit 1
fi

echo "Installing $BINARY to $BINDIR/"
install -d "$BINDIR"
install -m 0755 "$BINARY" "$BINDIR/$BINARY"

echo "Installing configuration to $CONFDIR/"
install -d "$CONFDIR"
echo "Installing broadcast template to $CONFDIR/"
if [ -f "$CONFDIR/broadcast.msg" ]; then
    echo "  $CONFDIR/broadcast.msg already exists, not overwriting"
else
    install -m 0644 "tpl/broadcast.msg" "$CONFDIR/broadcast.msg"
    echo "  installed $CONFDIR/broadcast.msg"
fi

if [ -f "$CONFDIR/$CONF" ]; then
    echo "  $CONFDIR/$CONF already exists, not overwriting"
    echo "  (new example installed as $CONFDIR/$CONF_EXAMPLE)"
    install -m 0600 "$CONF_EXAMPLE" "$CONFDIR/$CONF_EXAMPLE"
else
    install -m 0600 "$CONF_EXAMPLE" "$CONFDIR/$CONF"
    echo "  installed $CONFDIR/$CONF from example (mode 0600)"
fi

echo "Installing systemd unit to $UNITDIR/"
install -m 0644 "$UNIT" "$UNITDIR/$UNIT"

echo "Reloading systemd and enabling service"
systemctl daemon-reload
systemctl enable "$UNIT"

echo ""
echo "Installation complete.  Next steps:"
echo ""
echo "  1. Edit $CONFDIR/$CONF"
echo "     - Set iboot_address to your iBoot-G2+ IP"
echo "     - Set listen_address to this Pi's LAN IP"
echo "     - Set shared_username and shared_password to match"
echo "       the iBoot Graceful Shutdown page (Main outlet)"
echo ""
echo "  2. Verify EEPROM setting (one-time):"
echo "     sudo rpi-eeprom-config --edit"
echo "     Set: POWER_OFF_ON_HALT=1"
echo ""
echo "  3. Test in dry-run mode first:"
echo "     sudo $BINDIR/$BINARY --config $CONFDIR/$CONF --dry-run --verbose"
echo "     Then trigger Off from the iBoot web UI and check the log."
echo ""
echo "  4. When validated, start the service:"
echo "     sudo systemctl start $UNIT"
echo ""
echo "  5. Configure iBoot timing:"
echo "     - Hard Shutdown Delay: 45-60 seconds"
echo "     - Hard Reboot Delay: 45-60 seconds"
echo "     These must exceed the Pi's worst-case halt time (10-20 s)."
