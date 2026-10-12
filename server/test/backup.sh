#!/usr/bin/env bash
# Backup and restore of the database (docker-compose.cloud.yml, backup.sh):
# the service writes backups by itself, one of them undoes later changes, and
# after the loss of every volume a copy of it brings the server back with its
# key - the devices notice nothing.
#
#   ./test/backup.sh       (needs docker with compose, curl, python3; ports 8443 and 28080)
set -euo pipefail
cd "$(dirname "$0")/.."

export COMPOSE_PROJECT_NAME=rs232c2-backup
export C2_ADMIN_TOKEN="backup-$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
export C2_POLL_S=1 C2_BIND=127.0.0.1
export C2_DOMAIN=c2.test C2_PUBLIC_URL=https://c2.test:8443
export C2_CADDY_TLS=internal C2_HTTPS_PORT=8443 C2_HTTP_PORT=28080
export C2_DB_PASSWORD="$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
export C2_BACKUP_KEEP=2
CLOUD="docker compose -f docker-compose.cloud.yml"
API=https://c2.test:8443
TMP=

step() { printf '\n== %s\n' "$*"; }
fail() { echo "FAILED: $*" >&2; $CLOUD logs --tail 30 >&2 || true; exit 1; }
cleanup() {
  $CLOUD down -v --remove-orphans >/dev/null 2>&1 || true
  docker volume rm -f ${COMPOSE_PROJECT_NAME}_dev ${COMPOSE_PROJECT_NAME}_dev2 >/dev/null 2>&1 || true
  [ -z "$TMP" ] || rm -rf "$TMP"
}
trap cleanup EXIT
cleanup
TMP=$(mktemp -d)

# -k: the certificate is not the subject here (cloud.sh), and Caddy's authority is lost with the volumes
c() { curl -sSk --resolve c2.test:8443:127.0.0.1 "$@"; }
admin() { # METHOD PATH [JSON]  -> body, then the HTTP status on the last line
  c -X "$1" "$API$2" -H "Authorization: Bearer $C2_ADMIN_TOKEN" ${3:+-H 'Content-Type: application/json' -d "$3"} -w '\n%{http_code}'
}
json() { python3 -c "import sys,json; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }
devsim() {
  docker run --rm --network host --add-host c2.test:127.0.0.1 -v ${DEVVOL:-${COMPOSE_PROJECT_NAME}_dev}:/data \
         --entrypoint /devsim rs232-c2 -state /data/device.json -tls-skip-verify "$@"
}
expect() { [ "$2" = "$3" ] || fail "$1: got '$2', expected '$3'"; echo "ok   $1: $2"; }
up() { for i in $(seq 1 90); do c "$API/healthz" 2>/dev/null | grep -q ok && return; sleep 1; done; fail "server did not come up"; }
devices() { admin GET /admin/v1/devices | head -n -1 | json "sorted((x['name'],x['state']) for x in d)"; }
key() { admin GET /admin/v1/server | head -n -1 | json 'd["fingerprint"]'; }
bk() { $CLOUD exec -T backup /bin/sh /backup.sh "$@"; }
dumps() { $CLOUD exec -T backup sh -c 'ls /backups | grep "^c2-.*\.dump$" || true'; }
enroll() { # NAME -> ID
  local token out id code
  token=$(admin POST /admin/v1/enroll-tokens '{"ttl_s":120}' | head -n -1 | json 'd["token"]')
  out=$(devsim enroll -name "$1" "$token") || fail "devsim enroll"
  id=$(echo "$out" | awk '/^ID/{print $2}'); code=$(echo "$out" | awk '/^CODE/{print $2}')
  [ "$(admin POST /admin/v1/devices/$id/approve "{\"sas\":\"$code\"}" | tail -1)" = 200 ] || fail "approve $1"
  echo "$id"
}

step "a server with one device"
$CLOUD up -d --build --quiet-pull 2>&1 | tail -1
up
A=$(enroll geraet-a)
CMD=$(admin POST /admin/v1/devices/$A/commands '{"type":"ping"}' | head -n -1 | json 'd["id"]')
devsim run -n 1 -interval 1s >/dev/null
KEY=$(key)
expect "devices" "$(devices)" "[('geraet-a', 'active')]"

step "the service backs up by itself"
# Its first try comes with the start, before or after the server made the tables; a restart tries again.
$CLOUD restart backup 2>&1 | tail -1
for i in $(seq 1 30); do [ -n "$(dumps)" ] && break; sleep 1; done
[ "$(dumps | wc -l)" = 1 ] || fail "expected one backup, got: $(dumps)"
echo "ok   $(dumps)"
expect "only its owner reads it" "$($CLOUD exec -T backup stat -c %a /backups/$(dumps))" 600
bk check || fail "check does not see the fresh backup"
echo "ok   health check"
expect "no second one within the interval" "$($CLOUD restart backup >/dev/null 2>&1; sleep 3; dumps | wc -l)" 1

step "a backup on demand"
sleep 1; bk once | tail -1
FIRST=$(dumps | tail -1)
$CLOUD cp backup:/backups/$FIRST "$TMP/$FIRST" >/dev/null 2>&1 || fail "cannot copy the backup out"
echo "ok   copied off the machine ($(wc -c <"$TMP/$FIRST") bytes)"

step "things change after the backup"
B=$(DEVVOL=${COMPOSE_PROJECT_NAME}_dev2 enroll geraet-b)
expect "revoke the first device" "$(admin POST /admin/v1/devices/$A/revoke | tail -1)" 200
expect "devices" "$(devices)" "[('geraet-a', 'revoked'), ('geraet-b', 'active')]"

step "restore puts the earlier state back"
if OUT=$(bk restore "$FIRST" 2>&1); then fail "restore ran under a running server"; fi
echo "$OUT" | grep -q "stop it first" || fail "restore under a running server: $OUT"
echo "ok   refused while the server runs"
$CLOUD stop c2server 2>&1 | tail -1
echo garbage >"$TMP/c2-bad.dump"
$CLOUD cp "$TMP/c2-bad.dump" backup:/backups/c2-bad.dump >/dev/null 2>&1 || true   # read-only container: goes through the volume
$CLOUD cp "$TMP/c2-bad.dump" backup:/backups/c2-bad.dump >/dev/null 2>&1 || fail "cannot copy a file into the backups"
if bk restore c2-bad.dump >/dev/null 2>&1; then fail "a broken archive was restored"; fi
$CLOUD exec -T backup rm /backups/c2-bad.dump
expect "a broken archive changes nothing" "$($CLOUD exec -T db psql -U c2 -d c2 -Atc 'SELECT count(*) FROM devices')" 2
bk restore "$FIRST" | tail -1
$CLOUD start c2server 2>&1 | tail -1
up
expect "devices" "$(devices)" "[('geraet-a', 'active')]"
expect "server key" "$(key)" "$KEY"
expect "the command and its answer" "$(admin GET /admin/v1/devices/$A/commands | head -n -1 | json "[(c['id'],c['result']) for c in d]")" "[($CMD, 'pong')]"
expect "the device talks to the server" "$(devsim run -n 1 | head -1)" "STATE active"

step "only the newest are kept"
sleep 1; bk once | tail -1
sleep 1; bk once >/dev/null
expect "backups with C2_BACKUP_KEEP=2" "$(dumps | wc -l)" 2
dumps | grep -q "$FIRST" && fail "the oldest backup is still there"
echo "ok   the oldest is gone"

step "every volume lost: back from the copy"
$CLOUD down -v 2>&1 | tail -1
$CLOUD up -d db backup 2>&1 | tail -1          # not the server: it would make itself a new key
$CLOUD cp "$TMP/$FIRST" backup:/backups/$FIRST >/dev/null 2>&1 || fail "cannot copy the backup in"
bk restore "$FIRST" | tail -1
$CLOUD up -d 2>&1 | tail -1
up
expect "server key" "$(key)" "$KEY"
expect "devices" "$(devices)" "[('geraet-a', 'active')]"
expect "the device talks to the server" "$(devsim run -n 1 | head -1)" "STATE active"
CMD=$(admin POST /admin/v1/devices/$A/commands '{"type":"ping"}' | head -n -1 | json 'd["id"]')
devsim run -n 1 -interval 1s >/dev/null
expect "a new command" "$(admin GET /admin/v1/devices/$A/commands | head -n -1 | json "[c['result'] for c in d if c['id']==$CMD]")" "['pong']"

step "containers"
expect "backup has no way out" "$(docker inspect -f '{{range $k, $v := .NetworkSettings.Networks}}{{$k}} {{end}}' $($CLOUD ps -q backup))" "${COMPOSE_PROJECT_NAME}_back "
expect "and no privileges" "$(docker inspect -f '{{.HostConfig.CapDrop}} {{.HostConfig.ReadonlyRootfs}}' $($CLOUD ps -q backup))" "[ALL] true"

printf '\nALL BACKUP CHECKS PASSED\n'
