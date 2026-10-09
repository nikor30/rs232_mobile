#!/usr/bin/env bash
# End-to-end test against the real container: builds the image, starts the
# server on an empty volume, enrolls a simulated device, approves it, sends a
# command, restarts the server and revokes the device.
#
#   ./test/e2e.sh        (needs docker with compose, curl, python3)
set -euo pipefail
cd "$(dirname "$0")/.."

export COMPOSE_PROJECT_NAME=rs232c2-e2e
export C2_ADMIN_TOKEN="e2e-$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
export C2_POLL_S=1
API=https://127.0.0.1:8443
NET=${COMPOSE_PROJECT_NAME}_default

step() { printf '\n== %s\n' "$*"; }
fail() { echo "FAILED: $*" >&2; docker compose logs --tail 30 c2server >&2 || true; exit 1; }
cleanup() { docker compose down -v --remove-orphans >/dev/null 2>&1 || true; docker volume rm -f ${COMPOSE_PROJECT_NAME}_dev ${COMPOSE_PROJECT_NAME}_dev2 >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup

admin() { # METHOD PATH [JSON]  -> body, then the HTTP status on the last line
  curl -sk -X "$1" "$API$2" -H "Authorization: Bearer $C2_ADMIN_TOKEN" \
       ${3:+-H 'Content-Type: application/json' -d "$3"} -w '\n%{http_code}'
}
json() { python3 -c "import sys,json; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }
devsim() { # the simulator runs in its own container on the compose network
  docker run --rm --network "$NET" -v ${DEVVOL:-${COMPOSE_PROJECT_NAME}_dev}:/data \
         --entrypoint /devsim rs232-c2 -state /data/device.json -tls-skip-verify "$@"
}
expect() { [ "$2" = "$3" ] || fail "$1: got '$2', expected '$3'"; echo "ok   $1: $2"; }

step "unit tests (docker build --target test)"
docker build -q --target test . >/dev/null || fail "unit tests"
echo "ok   unit tests"

step "build and start"
docker compose up -d --build --quiet-pull 2>&1 | tail -3
for i in $(seq 1 30); do curl -sk "$API/healthz" 2>/dev/null | grep -q ok && break; sleep 1; done
curl -sk "$API/healthz" | grep -q ok || fail "server did not come up"

step "admin API needs the token"
expect "without token" "$(curl -sk -o /dev/null -w '%{http_code}' $API/admin/v1/devices)" 401

step "enroll"
TOKEN=$(admin POST /admin/v1/enroll-tokens '{"ttl_s":120,"note":"e2e"}' | head -n -1 | json 'd["token"]')
OUT=$(devsim enroll -name e2e-device "$TOKEN") || fail "devsim enroll"
ID=$(echo "$OUT" | awk '/^ID/{print $2}'); CODE=$(echo "$OUT" | awk '/^CODE/{print $2}')
echo "device $ID shows code $CODE"
state() { admin GET /admin/v1/devices | head -n -1 | json "[x['state'] for x in d if x['id']=='$ID'][0]"; }
expect "state after enroll" "$(state)" pending
expect "code not in the API" "$(admin GET /admin/v1/devices | grep -c "$CODE" || true)" 0
expect "pending device polls" "$(devsim run -n 1 | head -1)" "STATE pending"

step "the token works once"
if DEVVOL=${COMPOSE_PROJECT_NAME}_dev2 devsim enroll "$TOKEN" >/dev/null 2>&1; then fail "token accepted twice"; fi
echo "ok   second enrollment refused"

step "approve"
WRONG=$(printf '%06d' $(( (10#$CODE + 1) % 1000000 )))
expect "wrong code" "$(admin POST /admin/v1/devices/$ID/approve "{\"sas\":\"$WRONG\"}" | tail -1)" 403
expect "right code" "$(admin POST /admin/v1/devices/$ID/approve "{\"sas\":\"$CODE\"}" | tail -1)" 200
expect "state after approve" "$(state)" active

step "command round trip"
CMD=$(admin POST /admin/v1/devices/$ID/commands '{"type":"ping"}' | head -n -1 | json 'd["id"]')
RUN=$(devsim run -n 1 -interval 1s)
echo "$RUN" | sed 's/^/     /'
echo "$RUN" | grep -q "^CMD $CMD ping" || fail "device did not get the command"
expect "command result" "$(admin GET /admin/v1/devices/$ID/commands | head -n -1 | json '[(c["state"],c["result"]) for c in d][0]')" "('done', 'pong')"

step "server restart keeps devices"
docker compose restart c2server >/dev/null 2>&1
for i in $(seq 1 30); do curl -sk "$API/healthz" 2>/dev/null | grep -q ok && break; sleep 1; done
expect "state after restart" "$(state)" active
expect "device reconnects" "$(devsim run -n 1 | head -1)" "STATE active"

step "revoke"
expect "revoke" "$(admin POST /admin/v1/devices/$ID/revoke | tail -1)" 200
if devsim run -n 1 >/dev/null 2>&1; then fail "revoked device was served"; fi
echo "ok   revoked device refused"

step "container"
expect "simulator is in the image" "$(docker compose exec -T c2server /devsim 2>&1 | head -1 | cut -c1-5)" "usage"
expect "user" "$(docker inspect -f '{{.Config.User}}' $(docker compose ps -q c2server))" "10001:10001"

printf '\nALL END-TO-END CHECKS PASSED\n'
