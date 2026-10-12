#!/bin/sh
# Backups of the database (docker-compose.cloud.yml, service "backup"): one
# pg_dump archive per interval in /backups, the oldest ones removed.
#
#   loop            what the service runs: a backup whenever the newest is too old
#   once            a backup now
#   check           fails if the newest backup is older than two intervals (health check)
#   list            the backups there are
#   restore FILE    put FILE back - the server must be stopped (README.md)
#
# An archive holds everything the database does: the server's private key, the
# password hashes and the TOTP secrets. It is not encrypted.
set -eu

DIR=/backups
KEEP=${C2_BACKUP_KEEP:-14}
EVERY_H=${C2_BACKUP_EVERY_H:-24}
export PGHOST=${PGHOST:-db} PGUSER=${PGUSER:-c2} PGDATABASE=${PGDATABASE:-c2}
LOCK=1920152115            # pgLock in internal/store/postgres.go: held by a running server

say() { echo "backup: $*"; }
case "$KEEP$EVERY_H" in *[!0-9]*|'') say "C2_BACKUP_KEEP and C2_BACKUP_EVERY_H must be numbers"; exit 2 ;; esac
[ "$KEEP" -ge 1 ] && [ "$EVERY_H" -ge 1 ] || { say "C2_BACKUP_KEEP and C2_BACKUP_EVERY_H must be at least 1"; exit 2; }

newer() { # MINUTES: is there a backup younger than that?
  [ -n "$(find "$DIR" -maxdepth 1 -name 'c2-*.dump' -mmin "-$1" 2>/dev/null | head -n 1)" ]
}

once() {
  umask 077
  name="c2-$(date -u +%Y%m%dT%H%M%SZ).dump"
  rm -f "$DIR"/*.tmp
  # One snapshot of the whole database; the server may keep running.
  pg_dump -Fc -f "$DIR/$name.tmp"
  # Only an archive that reads back and holds the server key counts.
  pg_restore -l "$DIR/$name.tmp" | grep -q 'TABLE DATA public server ' || { say "$name does not hold the table server"; rm -f "$DIR/$name.tmp"; return 1; }
  mv "$DIR/$name.tmp" "$DIR/$name"
  say "wrote $name ($(wc -c <"$DIR/$name") bytes)"
  # names sort by time: everything but the newest $KEEP goes
  ls -1 "$DIR" | grep '^c2-.*\.dump$' | sort -r | tail -n "+$((KEEP + 1))" | while read -r old; do
    rm -f "$DIR/$old"; say "removed $old"
  done
}

case "${1:-loop}" in
loop)
  say "every $EVERY_H h, keeping $KEEP"
  while :; do
    newer $((EVERY_H * 60)) || once || say "FAILED - next try in 5 minutes"
    sleep 300 &
    wait $!                # so that a stop signal ends the loop at once
  done ;;
once)
  once ;;
check)
  newer $((EVERY_H * 120)) ;;
list)
  ls -l "$DIR" | grep 'c2-.*\.dump$' || say "none yet" ;;
restore)
  f=${2:?restore needs the name of a backup (see: list)}
  [ -f "$f" ] || f="$DIR/$f"
  [ -f "$f" ] || { say "no such backup: $2"; exit 1; }
  # a database that was just created restarts once before it is there for good
  i=0; until psql -Atc 'SELECT 1' >/dev/null 2>&1; do i=$((i + 1)); [ $i -lt 30 ] || break; sleep 1; done
  # A running server holds the state in memory and would write it over what
  # comes back here.
  held=$(psql -Atc "SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND classid = $LOCK") || { say "cannot reach the database"; exit 1; }
  [ "$held" = 0 ] || { say "the server is running on this database: stop it first"; exit 1; }
  # All or nothing: a failed restore leaves the database as it was.
  pg_restore --clean --if-exists --no-owner --single-transaction --exit-on-error -d "$PGDATABASE" "$f"
  say "restored $(basename "$f"): $(psql -Atc 'SELECT count(*) FROM devices') devices, $(psql -Atc 'SELECT count(*) FROM users') accounts" ;;
*)
  say "use loop, once, check, list or restore FILE"; exit 2 ;;
esac
