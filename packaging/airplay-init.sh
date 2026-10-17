#!/bin/sh
# /var/lib/webosbrew/init.d/airplay
# Starts the uxplay-daemon background service for always-on AirPlay discovery.
APP=/media/developer/apps/usr/palm/applications/com.github.danlflh.uxplaywebos
DAEMON=$APP/uxplay-daemon
PIDFILE=/tmp/uxplay-daemon.pid

if [ ! -x "$DAEMON" ]; then
    exit 0
fi

chown -R 6239:5000 "$APP/state" "$APP/daemon.log" "$APP/daemon.log.previous" 2>/dev/null || true
chmod 755 "$APP/state" 2>/dev/null || true

start-stop-daemon -S -b -m -p "$PIDFILE" -x "$DAEMON"
