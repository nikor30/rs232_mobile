#!/usr/bin/env bash
# Runs the firmware's protocol code (firmware/src/c2_proto.cpp, c2_console.cpp
# and c2_update.cpp, with the vendored ML-KEM and ML-DSA) on this machine
# against the real server container: known-answer self-test, enrollment,
# approval, a command and its result, a remote console, a signed firmware update.
#
#   tests/c2/run.sh     (needs docker, curl, python3, openssl 3.5+ and ArduinoJson
#                        from a firmware build: firmware/.pio/libdeps/...)
set -euo pipefail
cd "$(dirname "$0")/../.."

AJ=firmware/.pio/libdeps/waveshare-s3-lcd2/ArduinoJson/src
[ -f "$AJ/ArduinoJson.h" ] || { echo "ArduinoJson missing: build the firmware once (pio run)"; exit 1; }

P=rs232c2-fwhost
TOKEN_ADMIN="fwhost-$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
API=http://127.0.0.1:18080
fail() { echo "FAILED: $*" >&2; docker logs --tail 20 $P-server >&2 2>/dev/null || true; exit 1; }
LOG=$(mktemp)
cleanup() { rm -f "$LOG"; docker rm -f $P-server $P-console >/dev/null 2>&1 || true; docker network rm $P >/dev/null 2>&1 || true; docker volume rm -f $P-build >/dev/null 2>&1 || true; }
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
  D=firmware/lib/mldsa_native/src/mldsa
  for f in $M/mlkem/*.c $M/mlkem/fips202/*.c; do gcc -O2 -Wall -c "$f" -o /build/$(basename "$f").o; done
  for f in $D/src/*.c $D/src/fips202/*.c; do gcc -O2 -Wall -I$D -c "$f" -o /build/mldsa-$(basename "$f").o; done
  g++ -std=c++17 -O1 -Wall -Wextra -Ifirmware/src -I$M -I$D -I'"$AJ"' \
      tests/c2/host_client.cpp firmware/src/c2_proto.cpp firmware/src/c2_console.cpp firmware/src/c2_update.cpp /build/*.o -lmbedcrypto -o /build/c2host' || fail "compile"
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

echo "== remote console"
# The firmware's stream code plays the port: 2000 characters of greeting, an
# echo, "[BREAK]". Every third answer of the server is thrown away and asked
# for again, so the offsets have to do their work.
CON=$(admin POST /admin/v1/devices/$ID/commands '{"type":"console","args":{"port":1,"minutes":5}}' | json 'd["id"]')
docker run -d --name $P-console --network $P -v $P-build:/build $P-env /build/c2host console /build/device.bin >/dev/null
constate() { admin GET /admin/v1/devices/$ID/console | json 'd["console"]["state"]'; }
for i in $(seq 1 40); do [ "$(constate)" = open ] && break; sleep 0.5; done
expect "console opens after the device's yes" "$(constate)" open
output() { # EXPECTED-PYTHON-EXPRESSION -> "<n> characters, as sent", or what is wrong
  for i in $(seq 1 20); do
    R=$(admin GET "/admin/v1/devices/$ID/console/$CON/output?from=0&wait=1" | python3 -c "
import sys,json,base64
got=base64.b64decode(json.load(sys.stdin)['d'] or ''); want=$1
print('%d characters, as sent' % len(got) if got==want else 'short' if want.startswith(got) else 'WRONG: %r' % got[-40:])")
    [ "$R" = short ] || break
  done
  echo "$R"
}
GREETING="b'0123456789'*200"
expect "output of the port" "$(output "$GREETING")" "2000 characters, as sent"
admin POST /admin/v1/devices/$ID/console/$CON/input '{"d":"c2hvdw0="}' >/dev/null       # show\r
expect "typed and echoed" "$(output "$GREETING+b'show\\r'")" "2005 characters, as sent"
admin POST /admin/v1/devices/$ID/console/$CON/input '{"brk":true}' >/dev/null
expect "BREAK" "$(output "$GREETING+b'show\\r[BREAK]'")" "2012 characters, as sent"
admin POST /admin/v1/devices/$ID/console/$CON/close >/dev/null
[ "$(timeout 30 docker wait $P-console)" = 0 ] || { docker logs $P-console >&2; fail "console client"; }
docker logs $P-console >"$LOG" 2>&1
expect "the device's side" "$(tr '\n' ' ' <"$LOG")" "CONSOLE $CON open CLOSED closed by the administrator TYPED 73686f770d BRK 1 "
expect "session log" "$(admin GET /admin/v1/devices/$ID/console | json '[(s["reason"],s["bytes_in"],s["bytes_out"]) for s in d["sessions"]][-1]')" "('closed by the administrator', 5, 2012)"

echo "== the server changes its key"
OLDFP=$(admin GET /admin/v1/server | json 'd["fingerprint"]')
NEWFP=$(admin POST /admin/v1/server/rotate-key '{"days":1}' | json 'd["fingerprint"]')
[ "$NEWFP" != "$OLDFP" ] && [ ${#NEWFP} = 64 ] || fail "rotate-key: $NEWFP"
old() { admin GET /admin/v1/devices | json "[x.get('old_key', False) for x in d if x['id']=='$ID'][0]"; }
expect "the server waits for the device" "$(old)" True
# the tag of a key: the first eight bytes of its fingerprint
expect "first poll: the new key is announced on the old one" "$(host /build/c2host poll /build/device.bin | tr '\n' ' ')" "STATE active KEY announced ${NEWFP:0:16} "
expect "the server still waits" "$(old)" True
expect "second poll: the device says it has the key" "$(host /build/c2host poll /build/device.bin | tr '\n' ' ')" "STATE active "
expect "the server takes the device for changed" "$(old)" False
CMD=$(admin POST /admin/v1/devices/$ID/commands '{"type":"ping"}' | json 'd["id"]')
expect "third poll: the old key fails, the new one is pinned" "$(host /build/c2host poll /build/device.bin | tr '\n' ' ')" "KEY changed ${NEWFP:0:16} STATE active CMD $CMD ping "
expect "fourth poll: nothing left to do" "$(host /build/c2host poll /build/device.bin | tr '\n' ' ')" "STATE active "
expect "end of the change" "$(admin POST /admin/v1/server/rotate-key/end | json 'd["key_change"]')" None
expect "the device stays" "$(host /build/c2host poll /build/device.bin | tr '\n' ' ')" "STATE active "

echo "== signed firmware update"
# A key made here, for this run only; packages built the way tools/fw_sign.py
# builds them. The device code has to take the good one and nothing else.
W=$(mktemp -d); trap 'rm -rf "$W"; cleanup' EXIT
openssl genpkey -algorithm ML-DSA-65 -out $W/key.pem 2>/dev/null || fail "openssl cannot make an ML-DSA-65 key (3.5 or newer needed)"
openssl genpkey -algorithm ML-DSA-65 -out $W/other.pem 2>/dev/null
openssl pkey -in $W/key.pem -pubout -outform DER | tail -c 1952 >$W/pub.bin
docker run --rm -v $P-build:/build -v $W:/w:ro $P-env cp /w/pub.bin /build/pub.bin
head -c 70000 /dev/urandom >$W/image.bin
pkg() { # NAME KEY SEQ BOARD [tamper]
  python3 - "$W" "$@" <<'PY'
import sys, json, hashlib, base64, subprocess
w, name, key, seq, board = sys.argv[1:6]
image = open(w + "/image.bin", "rb").read()
manifest = json.dumps({"product": "rs232-console", "board": board, "fw": "9.9.9", "lang": "en", "seq": int(seq),
                       "size": len(image), "sha256": hashlib.sha256(image).hexdigest()}, separators=(",", ":")).encode()
open(w + "/m", "wb").write(manifest)
subprocess.run(["openssl", "pkeyutl", "-sign", "-inkey", w + "/" + key, "-in", w + "/m", "-out", w + "/s",
                "-pkeyopt", "context-string:rs232-console firmware"], check=True)
if len(sys.argv) > 6:      # the manifest changed after signing: another version, same length
    manifest = manifest.replace(b"9.9.9", b"6.6.6")
open(w + "/" + name, "wb").write(b"RSFW1\n" + manifest + b"\n" + base64.b64encode(open(w + "/s", "rb").read()) + b"\n" + image)
PY
}
update() { # PACKAGE INSTALLED-SEQ -> what the device code says
  local FW CMD
  FW=$(curl -s -X POST "$API/admin/v1/firmware" -H "Authorization: Bearer $TOKEN_ADMIN" --data-binary @"$W/$1" | json 'd["id"]')
  CMD=$(admin POST /admin/v1/devices/$ID/commands "{\"type\":\"update\",\"args\":{\"fw\":\"$FW\"}}" | json 'd["id"]')
  host /build/c2host update /build/device.bin /build/pub.bin "$2"
  admin DELETE /admin/v1/firmware/$FW >/dev/null
}
pkg good.rsfw key.pem 2000 waveshare-s3-lcd2
pkg forged.rsfw other.pem 2001 waveshare-s3-lcd2
pkg changed.rsfw key.pem 2002 waveshare-s3-lcd2 tamper
pkg board.rsfw key.pem 2003 some-other-board
expect "package signed with the owner's key" "$(update good.rsfw 1000)" "UPDATE ok 9.9.9 en 70000"
expect "result at the server" "$(admin GET /admin/v1/devices/$ID/commands | json '[(c["state"],c["result"]) for c in d][-1]')" "('done', 'updated to 9.9.9 (en), restarting')"
expect "the same package once it is installed" "$(update good.rsfw 2000)" "UPDATE refused: the package is not newer than what this device has installed"
expect "package signed with another key" "$(update forged.rsfw 1000)" "UPDATE refused: the signature does not match the key of this device"
expect "manifest changed after signing" "$(update changed.rsfw 1000)" "UPDATE refused: the signature does not match the key of this device"
expect "package for another board" "$(update board.rsfw 1000)" "UPDATE refused: the package is for another board"

printf '\nFIRMWARE PROTOCOL CODE INTEROPERATES WITH THE SERVER\n'
