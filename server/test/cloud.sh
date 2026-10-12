#!/usr/bin/env bash
# The set-up for the cloud (docker-compose.cloud.yml) on this machine: Caddy
# with a certificate of its own authority in front, the state in PostgreSQL.
# Starts a server on the JSON file first, so that the move into the database
# is part of the test.
#
#   ./test/cloud.sh        (needs docker with compose, curl, openssl, python3; ports 8443 and 28080)
set -euo pipefail
cd "$(dirname "$0")/.."

export COMPOSE_PROJECT_NAME=rs232c2-cloud
export C2_ADMIN_TOKEN="cloud-$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
export C2_POLL_S=1 C2_BIND=127.0.0.1
export C2_DOMAIN=c2.test C2_PUBLIC_URL=https://c2.test:8443
export C2_CADDY_TLS=internal C2_HTTPS_PORT=8443 C2_HTTP_PORT=28080
export C2_DB_PASSWORD="$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
export C2_TLS_HOSTS=c2.test
CLOUD="docker compose -f docker-compose.cloud.yml"
API=https://c2.test:8443
TMP=
CA=()                      # how curl checks the certificate: not at all first, then against Caddy's authority

step() { printf '\n== %s\n' "$*"; }
fail() { echo "FAILED: $*" >&2; $CLOUD logs --tail 30 >&2 || true; exit 1; }
cleanup() {
  docker rm -f ${COMPOSE_PROJECT_NAME}-sim >/dev/null 2>&1 || true
  $CLOUD down -v --remove-orphans >/dev/null 2>&1 || true
  docker compose down -v --remove-orphans >/dev/null 2>&1 || true
  docker volume rm -f ${COMPOSE_PROJECT_NAME}_dev ${COMPOSE_PROJECT_NAME}_dev2 >/dev/null 2>&1 || true
  [ -z "$TMP" ] || rm -rf "$TMP"
}
trap cleanup EXIT
cleanup
TMP=$(mktemp -d)

c() { curl -sS --resolve c2.test:8443:127.0.0.1 --resolve c2.test:28080:127.0.0.1 "${CA[@]}" "$@"; }
admin() { # METHOD PATH [JSON]  -> body, then the HTTP status on the last line
  c -X "$1" "$API$2" -H "Authorization: Bearer $C2_ADMIN_TOKEN" ${3:+-H 'Content-Type: application/json' -d "$3"} -w '\n%{http_code}'
}
json() { python3 -c "import sys,json; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }
devsim() { # on the host's network: the device reaches the server the way the world does
  docker run --rm --network host --add-host c2.test:127.0.0.1 -v ${DEVVOL:-${COMPOSE_PROJECT_NAME}_dev}:/data \
         --entrypoint /devsim rs232-c2 -state /data/device.json -tls-skip-verify "$@"
}
expect() { [ "$2" = "$3" ] || fail "$1: got '$2', expected '$3'"; echo "ok   $1: $2"; }
up() { for i in $(seq 1 90); do c "$API/healthz" 2>/dev/null | grep -q ok && return; sleep 1; done; fail "server did not come up"; }
state() { admin GET /admin/v1/devices | head -n -1 | json "[x['state'] for x in d if x['id']=='$1'][0]"; }
totp() { # SECRET [STEPS AHEAD]
  python3 -c "import sys,hmac,hashlib,struct,time,base64
k=base64.b32decode(sys.argv[1]); t=int(time.time())//30+int(sys.argv[2])
h=hmac.new(k,struct.pack('>Q',t),hashlib.sha1).digest(); o=h[-1]&15
print('%06d'%((struct.unpack('>I',h[o:o+4])[0]&0x7fffffff)%1000000))" "$1" "${2:-0}"
}

step "a server on the JSON file, with one device"
CA=(-k)
docker compose up -d --build --quiet-pull 2>&1 | tail -1
up
TOKEN=$(admin POST /admin/v1/enroll-tokens '{"ttl_s":120,"note":"vorher"}' | head -n -1 | json 'd["token"]')
OUT=$(devsim enroll -name altes-geraet "$TOKEN") || fail "devsim enroll"
OLD=$(echo "$OUT" | awk '/^ID/{print $2}'); CODE=$(echo "$OUT" | awk '/^CODE/{print $2}')
expect "approve" "$(admin POST /admin/v1/devices/$OLD/approve "{\"sas\":\"$CODE\"}" | tail -1)" 200
CMD=$(admin POST /admin/v1/devices/$OLD/commands '{"type":"ping"}' | head -n -1 | json 'd["id"]')
devsim run -n 1 -interval 1s >/dev/null
KEY=$(admin GET /admin/v1/server | head -n -1 | json 'd["fingerprint"]')
docker compose down 2>&1 | tail -1          # the volume with state.json stays

step "the same state behind Caddy, in PostgreSQL"
$CLOUD up -d --quiet-pull 2>&1 | tail -1
up
$CLOUD logs c2server 2>&1 | grep -q "took over the state of the file" || fail "state.json was not imported"
echo "ok   state.json imported"
expect "device is still active" "$(state $OLD)" active
expect "server key is the same" "$(admin GET /admin/v1/server | head -n -1 | json 'd["fingerprint"]')" "$KEY"
expect "old command and its answer" "$(admin GET /admin/v1/devices/$OLD/commands | head -n -1 | json "[(c['id'],c['result']) for c in d]")" "[($CMD, 'pong')]"
expect "old device talks to the new set-up" "$(devsim run -n 1 | head -1)" "STATE active"
expect "rows in the database" "$($CLOUD exec -T db psql -U c2 -d c2 -Atc 'SELECT (SELECT count(*) FROM devices), (SELECT count(*) FROM commands), (SELECT count(*) FROM server)')" "1|1|1"

step "certificate and proxy"
for i in $(seq 1 30); do $CLOUD cp caddy:/data/caddy/pki/authorities/local/root.crt "$TMP/root.crt" >/dev/null 2>&1 && break; sleep 1; done
CA=(--cacert "$TMP/root.crt")
expect "certificate checks out for c2.test" "$(c "$API/healthz")" ok
# the firmware names the server by address in a LAN and then sends no server name
expect "certificate for a client without server name" "$(openssl s_client -connect 127.0.0.1:8443 -noservername </dev/null 2>/dev/null | openssl x509 -noout -ext subjectAltName | tail -1 | tr -d ' ')" "DNS:c2.test"
expect "http goes to https" "$(c -o /dev/null -w '%{http_code}' http://c2.test:28080/healthz)" 308
c -D - -o /dev/null "$API/" | grep -qi '^strict-transport-security: max-age=31536000' || fail "no HSTS header"
echo "ok   HSTS header"
HEAD=$(c -D - -o /dev/null "$API/v1/server-key")
echo "$HEAD" | grep -qi '^content-length:' || fail "the server key comes without Content-Length"
echo "$HEAD" | grep -qi '^transfer-encoding:' && fail "the server key comes in chunks"
echo "ok   answers for devices carry their length"
expect "database is not published" "$(docker inspect -f '{{len .NetworkSettings.Ports}}' $($CLOUD ps -q db))" 1
expect "nor reachable from this machine" "$(docker inspect -f '{{range .NetworkSettings.Ports}}{{len .}}{{end}}' $($CLOUD ps -q db))" 0
expect "server port is not published" "$(docker port $($CLOUD ps -q c2server) | wc -l)" 0

step "a new device through Caddy"
TOKEN=$(admin POST /admin/v1/enroll-tokens '{"ttl_s":120,"note":"neu"}' | head -n -1 | json 'd["token"]')
OUT=$(DEVVOL=${COMPOSE_PROJECT_NAME}_dev2 devsim enroll -name neues-geraet "$TOKEN") || fail "devsim enroll"
NEW=$(echo "$OUT" | awk '/^ID/{print $2}'); CODE=$(echo "$OUT" | awk '/^CODE/{print $2}')
expect "approve" "$(admin POST /admin/v1/devices/$NEW/approve "{\"sas\":\"$CODE\"}" | tail -1)" 200
CMD=$(admin POST /admin/v1/devices/$NEW/commands '{"type":"ping"}' | head -n -1 | json 'd["id"]')
RUN=$(DEVVOL=${COMPOSE_PROJECT_NAME}_dev2 devsim run -n 1 -interval 1s)
echo "$RUN" | grep -q "^CMD $CMD ping" || fail "device did not get the command"
expect "command result" "$(admin GET /admin/v1/devices/$NEW/commands | head -n -1 | json '[(c["state"],c["result"]) for c in d][0]')" "('done', 'pong')"

step "remote console through Caddy"
docker run -d --name ${COMPOSE_PROJECT_NAME}-sim --network host --add-host c2.test:127.0.0.1 -v ${COMPOSE_PROJECT_NAME}_dev2:/data \
       --entrypoint /devsim rs232-c2 -state /data/device.json -tls-skip-verify run -console -interval 1s >/dev/null
SESS=$(admin POST /admin/v1/devices/$NEW/commands '{"type":"console","args":{"port":1,"minutes":5}}' | head -n -1 | json 'd["id"]')
CON=/admin/v1/devices/$NEW/console
for i in $(seq 1 30); do [ "$(admin GET $CON | head -n -1 | json '(d["console"] or {}).get("state")')" = open ] && break; sleep 1; done
expect "console" "$(admin GET $CON | head -n -1 | json 'd["console"]["state"]')" open
sleep 2
NEXT=$(admin GET "$CON/$SESS/output?from=0&wait=5" | head -n -1 | json 'd["next"]')
[ "$NEXT" -gt 0 ] || fail "the port said nothing"
echo "ok   the port's greeting arrived: $NEXT characters"
expect "typing" "$(admin POST $CON/$SESS/input "{\"d\":\"$(printf 'show\r' | base64)\"}" | tail -1)" 200
sleep 3
OUT=$(admin GET "$CON/$SESS/output?from=$NEXT&wait=5" | head -n -1)
echo "$OUT" | json 'd["d"]' | base64 -d | grep -q show || fail "no echo: $OUT"
echo "ok   the echo came back"
NEXT=$(echo "$OUT" | json 'd["next"]')
# The browser waits up to 20 s for output in one request; Caddy must hold that open.
T0=$(date +%s)
expect "a long wait for output that does not come" "$(admin GET "$CON/$SESS/output?from=$NEXT&wait=20" | tail -1)" 200
T=$(( $(date +%s) - T0 )); [ $T -ge 18 ] && [ $T -le 25 ] || fail "the wait took $T s"
echo "ok   it took $T s"
expect "close" "$(admin POST $CON/$SESS/close | tail -1)" 200
for i in $(seq 1 20); do [ "$(admin GET $CON | head -n -1 | json 'd["sessions"][-1].get("reason")')" != None ] && break; sleep 1; done
expect "session on record" "$(admin GET $CON | head -n -1 | json '(d["sessions"][-1]["reason"], d["sessions"][-1]["bytes_in"])')" "('closed by the administrator', 5)"
docker rm -f ${COMPOSE_PROJECT_NAME}-sim >/dev/null

step "an account, signed in through Caddy"
SETUP=$(admin POST /admin/v1/users '{"name":"anna","kind":"local"}' | head -n -1 | json 'd["setup"]')
post() { c -X POST "$API$1" -H 'Content-Type: application/json' -H 'X-C2: 1' -d "$2" "${@:3}"; }
SECRET=$(post /auth/setup "{\"name\":\"anna\",\"setup\":\"$SETUP\",\"password\":\"ein langes Passwort 1\"}" | json 'd["secret"]')
expect "setup" "$(post /auth/setup/confirm "{\"name\":\"anna\",\"setup\":\"$SETUP\",\"code\":\"$(totp $SECRET)\"}" -c "$TMP/jar" -o /dev/null -w '%{http_code}')" 200
grep -q $'^#HttpOnly_c2.test\tFALSE\t/\tTRUE\t0\t__Host-c2\t' "$TMP/jar" || fail "no secure __Host- cookie: $(cat "$TMP/jar")"
echo "ok   cookie is __Host-c2, secure, HttpOnly"
expect "cookie opens the admin API" "$(c -b "$TMP/jar" -o /dev/null -w '%{http_code}' "$API/admin/v1/devices")" 200
expect "not for a write without X-C2" "$(c -b "$TMP/jar" -X POST -o /dev/null -w '%{http_code}' "$API/admin/v1/devices/$NEW/revoke")" 403

step "the sender's address"
GW=$(docker network inspect -f '{{(index .IPAM.Config 0).Gateway}}' ${COMPOSE_PROJECT_NAME}_front)
c -X POST "$API/auth/login" -H 'Content-Type: application/json' -H 'X-Forwarded-For: 203.0.113.9' -d '{"name":"mallory","password":"x","code":"000000"}' -o /dev/null
# The client's address as Caddy saw it - not Caddy's own, and not what the client claims.
expect "the failed sign-in is in the audit log" "$(admin GET '/admin/v1/audit?n=100' | head -n -1 | json "[(e['ok'],e['ip']) for e in d if e.get('target')=='mallory']")" "[(False, '$GW')]"
expect "and so is the account, from the same address" "$(admin GET '/admin/v1/audit?n=100' | head -n -1 | json "[e['ip'] for e in d if e['action']=='setup']")" "['$GW']"

step "one server per database"
if OUT=$($CLOUD run --rm --no-deps c2server 2>&1); then fail "a second server started"; fi
echo "$OUT" | grep -q "another server process is using this database" || fail "second server: $OUT"
echo "ok   a second server refuses to start"

step "the database goes away and comes back"
SRV=$($CLOUD ps -q c2server)
for i in $(seq 1 60); do [ "$(docker inspect -f '{{.State.Health.Status}}' $SRV)" = healthy ] && break; sleep 1; done
expect "container" "$(docker inspect -f '{{.State.Health.Status}}' $SRV)" healthy
expect "state" "$(admin GET /admin/v1/status | head -n -1 | json '(d["state"]["ok"], d["devices"]["active"])')" "(True, 2)"
$CLOUD stop db 2>&1 | tail -1
for i in $(seq 1 40); do [ "$(c -o /dev/null -w '%{http_code}' "$API/healthz")" = 503 ] && break; sleep 1; done
expect "healthz without the database" "$(c -o /dev/null -w '%{http_code}' "$API/healthz")" 503
if $CLOUD exec -T c2server /c2server -health 2>/dev/null; then fail "the health check passes without a database"; fi
echo "ok   the container's health check fails"
expect "status says why" "$(admin GET /admin/v1/status | head -n -1 | json '(d["state"]["ok"], bool(d["state"]["error"]))')" "(False, True)"
expect "a change is refused, not lost quietly" "$(admin POST /admin/v1/enroll-tokens '{"ttl_s":120,"note":"ohne-datenbank"}' | tail -1)" 500
for i in $(seq 1 40); do $CLOUD logs c2server 2>&1 | grep -q "state: CANNOT BE SAVED" && break; sleep 1; done
$CLOUD logs c2server 2>&1 | grep -q "state: CANNOT BE SAVED" || fail "nothing in the log about the lost database"
echo "ok   it is in the log"
$CLOUD start db 2>&1 | tail -1
for i in $(seq 1 60); do [ "$(c -o /dev/null -w '%{http_code}' "$API/healthz")" = 200 ] && break; sleep 1; done
expect "healthz with the database back" "$(c "$API/healthz")" ok
for i in $(seq 1 40); do $CLOUD logs c2server 2>&1 | grep -q "state: can be saved again" && break; sleep 1; done
$CLOUD logs c2server 2>&1 | grep -q "state: can be saved again" || fail "nothing in the log about the database being back"
echo "ok   and that it is back"
expect "devices are still there" "$(admin GET /admin/v1/status | head -n -1 | json '(d["state"]["ok"], d["devices"]["active"])')" "(True, 2)"
expect "old device talks to the server" "$(devsim run -n 1 | head -1)" "STATE active"
# the lock went with the database's connections; the server has taken it again
if OUT=$($CLOUD run --rm --no-deps c2server 2>&1); then fail "a second server started after the database came back"; fi
echo "$OUT" | grep -q "another server process is using this database" || fail "second server: $OUT"
echo "ok   a second server still refuses to start"

step "the server changes its key"
NEWKEY=$(admin POST /admin/v1/server/rotate-key '{"days":30}' | head -n -1 | json 'd["fingerprint"]')
[ "$NEWKEY" != "$KEY" ] || fail "the key is the same"
expect "the old key still counts, two devices wait" "$(admin GET /admin/v1/server | head -n -1 | json '(d["key_change"]["old_fingerprint"]=="'$KEY'", d["key_change"]["devices_waiting"])')" "(True, 2)"
expect "it is in the audit log" "$(admin GET '/admin/v1/audit?n=5' | head -n -1 | json "[e['ok'] for e in d if e['action'].endswith('rotate-key')]")" "[True]"
RUN=$(devsim run -n 2 -interval 1s | tr '\n' ' ')
expect "old device: told on the old key, then confirms" "$RUN" "KEY ${NEWKEY:0:16} announced STATE active STATE active "
expect "old device: next handshake on the new key" "$(devsim run -n 1 | tr '\n' ' ')" "KEY changed to ${NEWKEY:0:16} STATE active "
expect "one device still waits" "$(admin GET /admin/v1/server | head -n -1 | json 'd["key_change"]["devices_waiting"]')" 1
expect "both keys are in the database" "$($CLOUD exec -T db psql -U c2 -d c2 -Atc 'SELECT old_key IS NOT NULL, old_until > now(), (SELECT count(*) FROM devices WHERE old_key) FROM server')" "t|t|1"
# the device that has not come yet finds its old key still there after the restart below

step "restart without the admin token: accounts only"
$CLOUD down 2>&1 | tail -1
SAVED=$C2_ADMIN_TOKEN
C2_ADMIN_TOKEN= $CLOUD up -d 2>&1 | tail -1
up
$CLOUD logs c2server 2>&1 | grep -q "no admin token: accounts only" || fail "server did not say that it runs on accounts only"
expect "the token is worth nothing now" "$(c -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $SAVED" "$API/admin/v1/devices")" 401
expect "old cookie neither (sessions are in memory)" "$(c -b "$TMP/jar" -o /dev/null -w '%{http_code}' "$API/admin/v1/devices")" 401
# a code is taken once: the next time step's code is still within what the server accepts
expect "sign in" "$(post /auth/login "{\"name\":\"anna\",\"password\":\"ein langes Passwort 1\",\"code\":\"$(totp $SECRET 1)\"}" -c "$TMP/jar" -o /dev/null -w '%{http_code}')" 200
DEVS=$(c -b "$TMP/jar" "$API/admin/v1/devices")
expect "both devices survived" "$(echo "$DEVS" | json "sorted((x['name'],x['state']) for x in d)")" "[('altes-geraet', 'active'), ('neues-geraet', 'active')]"
expect "old device" "$(devsim run -n 1 | head -1)" "STATE active"
expect "new device: the old key survived the restart" "$(DEVVOL=${COMPOSE_PROJECT_NAME}_dev2 devsim run -n 1 | tr '\n' ' ')" "KEY ${NEWKEY:0:16} announced STATE active "
expect "revoke with the account" "$(c -b "$TMP/jar" -X POST -H 'X-C2: 1' -o /dev/null -w '%{http_code}' "$API/admin/v1/devices/$NEW/revoke")" 200
expect "who did it is in the database" "$($CLOUD exec -T db psql -U c2 -d c2 -Atc "SELECT who, ok FROM audit WHERE action LIKE '%revoke' ORDER BY seq DESC LIMIT 1")" "anna|t"

step "containers"
expect "server user" "$(docker inspect -f '{{.Config.User}}' $($CLOUD ps -q c2server))" "10001:10001"
expect "database runs as" "$($CLOUD exec -T db stat -c %U /proc/1)" "postgres"

printf '\nALL CLOUD CHECKS PASSED\n'
