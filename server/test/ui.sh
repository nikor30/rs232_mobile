#!/usr/bin/env bash
# Tests the administrator's web interface in a headless browser against the
# real container, with a simulated device on the other side.
#
#   ./test/ui.sh [DIR]      screenshots go to DIR (default: a temporary one)
#   UI_BROWSER=webkit ./test/ui.sh   the same in WebKit, the engine of Safari
# Needs docker; pulls mcr.microsoft.com/playwright/python on first use (3 GB).
set -euo pipefail
cd "$(dirname "$0")/.."

export COMPOSE_PROJECT_NAME=rs232c2-ui
export C2_ADMIN_TOKEN="ui-$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
export C2_POLL_S=1
NET=${COMPOSE_PROJECT_NAME}_default
PW=rs232c2-ui-env
OUT=$(mkdir -p "${1:-$(mktemp -d)}" && cd "${1:-$_}" && pwd)

fail() { echo "FAILED: $*" >&2; docker logs --tail 10 ${COMPOSE_PROJECT_NAME}-sim >&2 2>/dev/null || true; docker compose logs --tail 20 c2server >&2 || true; exit 1; }
cleanup() { docker rm -f ${COMPOSE_PROJECT_NAME}-sim >/dev/null 2>&1 || true; docker compose down -v --remove-orphans >/dev/null 2>&1 || true; docker volume rm -f ${COMPOSE_PROJECT_NAME}_dev >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup

python3 test/i18n_check.py || fail "texts without a translation"
# the browsers come with the image, the Python package does not
printf 'FROM mcr.microsoft.com/playwright/python:v1.63.0-noble\nRUN pip install -q playwright==1.63.0\n' | docker build -q -t $PW - >/dev/null
docker compose up -d --build --quiet-pull 2>&1 | tail -1
for i in $(seq 1 30); do curl -sk https://127.0.0.1:8443/healthz 2>/dev/null | grep -q ok && break; sleep 1; done
curl -sk https://127.0.0.1:8443/healthz | grep -q ok || fail "server did not come up"

browser() { # STAGE [ARG]
  docker run --rm --network "$NET" -e UI_BROWSER="${UI_BROWSER:-chromium}" -v "$PWD/test":/test:ro -v "$OUT":/out $PW \
    python /test/ui_test.py "$1" https://c2server:8443 "$C2_ADMIN_TOKEN" "${2:-}"
}
devsim() { docker run --rm --network "$NET" -v ${COMPOSE_PROJECT_NAME}_dev:/data --entrypoint /devsim rs232-c2 -state /data/device.json -tls-skip-verify "$@"; }

TOKEN=$(browser invite | tee /dev/stderr | awk '/^TOKEN/{print $2}') || fail "invite"
[ -n "$TOKEN" ] || fail "no token from the page"
# A device may call itself anything; the page must show it as text.
NAME='Labor <img src=x onerror=window.pwned=1>'
OUT1=$(devsim enroll -name "$NAME" "$TOKEN") || fail "devsim enroll"
CODE=$(echo "$OUT1" | awk '/^CODE/{print $2}')
browser approve "Labor:$CODE" || fail "approve"
docker run -d --name ${COMPOSE_PROJECT_NAME}-sim --network "$NET" -v ${COMPOSE_PROJECT_NAME}_dev:/data --entrypoint /devsim rs232-c2 \
  -state /data/device.json -tls-skip-verify run -control -console -unattended -interval 1s >/dev/null
browser server || fail "server"
browser command "Labor" || fail "command"
browser accounts || fail "accounts"
browser language "Labor" || fail "language"
browser phone "Labor" || fail "phone"
browser revoke "Labor" || fail "revoke"
echo "screenshots: $OUT"
printf '\nWEB INTERFACE CHECKS PASSED\n'
