#!/usr/bin/env bash
# Watchdog from outside: asks the control center's /healthz and looks at its
# certificate, and sends a push message through ntfy when that changes - the
# server cannot report that its own machine is gone. Run every two minutes by
# rs232-c2-watchdog.timer on a machine elsewhere.
#
# Settings: /etc/rs232-c2-watchdog.env or the environment.
#   C2_URL       e.g. https://control.nikoscloud.de (required)
#   C2_NTFY      ntfy topic URL, e.g. https://ntfy.sh/<unguessable name> (required)
#   C2_FAILS     failed checks in a row before a message (default 2)
#   C2_REMIND_H  repeat the message every N hours while it fails (default 12)
#   C2_CERT_DAYS warn when the certificate has fewer days left (default 14)
#   C2_STATE     where the watchdog remembers (default /var/lib/rs232-c2-watchdog)
#
#   watchdog.sh          one check
#   watchdog.sh test     send a test message
set -uo pipefail
[ -r /etc/rs232-c2-watchdog.env ] && . /etc/rs232-c2-watchdog.env
: "${C2_URL:?C2_URL is not set}" "${C2_NTFY:?C2_NTFY is not set}"
FAILS=${C2_FAILS:-2}; REMIND=$(( ${C2_REMIND_H:-12} * 3600 )); CERT_DAYS=${C2_CERT_DAYS:-14}
STATE=${C2_STATE:-/var/lib/rs232-c2-watchdog}
mkdir -p "$STATE"

send() { # PRIORITY TAGS MESSAGE
  curl -s -m 20 -H "Title: RS232 control center" -H "Priority: $1" -H "Tags: $2" -d "$3" "$C2_NTFY" >/dev/null
}
[ "${1:-}" = test ] && { send default white_check_mark "Test from the watchdog on $(hostname): $C2_URL"; exit; }

now=$(date +%s)
code=$(curl -s -m 20 -o "$STATE/body" -w '%{http_code}' "$C2_URL/healthz")
if [ "$code" = 200 ] && grep -q ok "$STATE/body"; then problem=""
elif [ "$code" = 000 ]; then problem="not reachable"
else problem="/healthz answers $code"; fi

# the certificate: Let's Encrypt renews it, unless something stops that
host=${C2_URL#https://}; host=${host%%/*}
end=$(echo | timeout 20 openssl s_client -servername "$host" -connect "$host:443" 2>/dev/null | openssl x509 -noout -enddate 2>/dev/null | cut -d= -f2)
if [ -n "$end" ]; then
  left=$(( ($(date -d "$end" +%s) - now) / 86400 ))
  [ "$left" -lt "$CERT_DAYS" ] && problem="${problem:+$problem; }certificate runs out in $left days"
fi

n=$(cat "$STATE/fails" 2>/dev/null || echo 0)
told=$(cat "$STATE/told" 2>/dev/null || echo 0)       # when the failure was last reported, 0 = not reported
if [ -z "$problem" ]; then
  [ "$told" != 0 ] && send default white_check_mark "$C2_URL is fine again"
  echo 0 > "$STATE/fails"; echo 0 > "$STATE/told"
  exit 0
fi
n=$((n + 1)); echo "$n" > "$STATE/fails"
echo "watchdog: $problem ($n in a row)" >&2
if [ "$n" -ge "$FAILS" ] && { [ "$told" = 0 ] || [ $((now - told)) -ge "$REMIND" ]; }; then
  send high rotating_light "$C2_URL: $problem (seen from $(hostname))" && echo "$now" > "$STATE/told"
fi
exit 1
