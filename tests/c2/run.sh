#!/usr/bin/env bash
# Runs the firmware's protocol code (firmware/src/c2_proto.cpp, with the vendored
# ML-KEM) on this machine against the real server container: known-answer
# self-test, enrollment, approval, a command and its result.
#
#   tests/c2/run.sh     (needs docker, curl, python3, and ArduinoJson from a
#                        firmware build: firmware/.pio/libdeps/...)
set -euo pipefail
cd "$(dirname "$0")/../.."

AJ=firmware/.pio/libdeps/waveshare-s3-lcd2/ArduinoJson/src
[ -f "$AJ/ArduinoJson.h" ] || { echo "ArduinoJson missing: build the firmware once (pio run)"; exit 1; }

P=rs232c2-fwhost
TOKEN_ADMIN="fwhost-$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
API=http://127.0.0.1:18080
fail() { echo "FAILED: $*" >&2; docker logs --tail 20 $P-server >&2 2>/dev/null || true; exit 1; }
cleanup() { docker rm -f $P-server >/dev/null 2>&1 || true; docker network rm $P >/dev/null 2>&1 || true; docker volume rm -f $P-build >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup
expect() { [ "$2" = "$3" ] || fail "$1: got '$2', expected '$3'"; echo "ok   $1: $2"; }
admin() { curl -s -X "$1" "$API$2" -H "Authorization: Bearer $TOKEN_ADMIN" ${3:+-d "$3"}; }
json() { python3 -c "import sys,json; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }

echo "== build"
docker build -q -t rs232-c2 server >/dev/null
docker build -q -t $P-env tests/c2 >/dev/null
docker network create $P >/dev/null
host() { docker run --rm --network $P -v "$PWD":/src:ro -v $P-build:/build -w /src $P-env "$@"; }
host sh -c '
  set -e
  M=firmware/lib/mlkem_native/src
  for f in $M/mlkem/*.c $M/mlkem/fips202/*.c; do gcc -O2 -Wall -c "$f" -o /build/$(basename "$f").o; done
  g++ -std=c++17 -O1 -Wall -Wextra -Ifirmware/src -I$M -I'"$AJ"' \
      tests/c2/host_client.cpp firmware/src/c2_proto.cpp /build/*.o -lmbedcrypto -o /build/c2host' || fail "compile"
echo "ok   firmware protocol code compiled against mbedTLS $(host sh -c "dpkg-query -W -f='\${Version}' libmbedtls-dev")"

echo "== known answers"
expect "self-test" "$(host /build/c2host selftest)" "self-test ok"

echo "== against the server"
docker run -d --name $P-server --network $P --network-alias c2server -p 127.0.0.1:18080:8080 \
  -e C2_ADMIN_TOKEN="$TOKEN_ADMIN" -e C2_TLS=off -e C2_LISTEN=:8080 -e C2_PUBLIC_URL=http://c2server:8080 -e C2_POLL_S=1 \
  rs232-c2 >/dev/null
for i in $(seq 1 30); do curl -s $API/healthz 2>/dev/null | grep -q ok && break; sleep 1; done
curl -s $API/healthz | grep -q ok || fail "server did not come up"

TOKEN=$(admin POST /admin/v1/enroll-tokens '{"ttl_s":120}' | json 'd["token"]')
OUT=$(host /build/c2host enroll /build/device.bin "$TOKEN") || fail "enroll"
ID=$(echo "$OUT" | awk '/^ID/{print $2}'); CODE=$(echo "$OUT" | awk '/^CODE/{print $2}')
echo "device $ID shows code $CODE"
expect "enroll answer" "$(echo "$OUT" | awk '/^STATE/{print $2}')" pending
expect "server knows the device" "$(admin GET /admin/v1/devices | json "[x['state'] for x in d if x['id']=='$ID'][0]")" pending
expect "approve with the device's code" "$(admin POST /admin/v1/devices/$ID/approve "{\"sas\":\"$CODE\"}" | json 'd["state"]')" active
CMD=$(admin POST /admin/v1/devices/$ID/commands '{"type":"ping"}' | json 'd["id"]')
OUT=$(host /build/c2host poll /build/device.bin) || fail "poll"
expect "poll" "$(echo "$OUT" | tr '\n' ' ')" "STATE active CMD $CMD ping "
expect "result at the server" "$(admin GET /admin/v1/devices/$ID/commands | json '[(c["state"],c["result"]) for c in d][0]')" "('done', 'pong from firmware code')"

# a token whose fingerprint names another server must be refused by the device code
BAD=$(admin POST /admin/v1/enroll-tokens '{}' | json 'd["token"]' | python3 -c "
import sys,json,base64
t=sys.stdin.read().strip()[5:]; d=json.loads(base64.urlsafe_b64decode(t+'='*(-len(t)%4)))
d['fp']=base64.b64encode(bytes(32)).decode()
print('c2e1:'+base64.urlsafe_b64encode(json.dumps(d).encode()).decode().rstrip('='))")
if host /build/c2host enroll /build/other.bin "$BAD" >/dev/null 2>&1; then fail "token for another server accepted"; fi
echo "ok   token naming another server key refused"

printf '\nFIRMWARE PROTOCOL CODE INTEROPERATES WITH THE SERVER\n'
