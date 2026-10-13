#!/usr/bin/env bash
# Signs in at the running test server the way a WLAN client does (eapol_test
# from wpa_supplicant): EAP-TLS with the device's certificate, PEAP-MSCHAPv2
# and EAP-TTLS with the user - and checks that the wrong ones are refused.
set -uo pipefail
cd "$(dirname "$0")"
[ -e data/secret ] || { echo "run ./setup.sh first"; exit 1; }
SECRET=$(cat data/secret)
PASS=$(sed -n 's/.*Cleartext-Password := "\(.*\)"/\1/p' data/authorize)
NET=$(docker inspect -f '{{range $k,$v := .NetworkSettings.Networks}}{{$k}}{{end}}' "$(docker compose ps -q radius)" 2>/dev/null)
[ -n "$NET" ] || { echo "the server is not running: docker compose up -d"; exit 1; }
docker image inspect rs232-eapol-test >/dev/null 2>&1 || docker build -q -t rs232-eapol-test - >/dev/null <<'DOCKER'
FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends eapoltest >/dev/null && rm -rf /var/lib/apt/lists/*
DOCKER
T=$(mktemp -d); trap 'rm -rf $T' EXIT
cp data/device/radius-ca.pem data/device/rs232-console.pem $T/
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -keyout $T/other.key -out $T/other.crt -subj "/CN=rs232-console" 2>/dev/null
cat $T/other.crt $T/other.key > $T/other.pem
chmod 644 $T/*; chmod 755 $T
conf() { printf 'network={\n key_mgmt=WPA-EAP\n ca_cert="/t/radius-ca.pem"\n%s\n}\n' "$2" > $T/$1.conf; }
conf tls      $' eap=TLS\n identity="rs232-console"\n client_cert="/t/rs232-console.pem"\n private_key="/t/rs232-console.pem"'
conf peap     $' eap=PEAP\n identity="rs232"\n password="'"$PASS"$'"\n phase2="auth=MSCHAPV2"'
conf ttls     $' eap=TTLS\n identity="rs232"\n password="'"$PASS"$'"\n phase2="auth=MSCHAPV2"'
conf ttlspap  $' eap=TTLS\n identity="rs232"\n password="'"$PASS"$'"\n phase2="auth=PAP"'
conf badpass  $' eap=PEAP\n identity="rs232"\n password="not-the-password"\n phase2="auth=MSCHAPV2"'
conf badcert  $' eap=TLS\n identity="rs232-console"\n client_cert="/t/other.pem"\n private_key="/t/other.pem"'
fail=0
try() {                                  # name, what is expected, text
  out=$(docker run --rm --network "$NET" -v $T:/t:ro rs232-eapol-test eapol_test -c /t/$1.conf -a "$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$(docker compose ps -q radius)")" -s "$SECRET" -t 8 2>&1 | tail -1)
  if [ "$out" = "$2" ]; then echo "ok   $3"; else echo "FAIL $3: $out"; fail=1; fi
}
try tls     SUCCESS "EAP-TLS with the device's certificate"
try peap    SUCCESS "PEAP-MSCHAPv2 with user and password"
try ttls    SUCCESS "EAP-TTLS (MSCHAPv2)"
try ttlspap SUCCESS "EAP-TTLS (PAP)"
try badpass FAILURE "wrong password is refused"
try badcert FAILURE "a certificate from another authority is refused"
[ $fail = 0 ] && echo && echo "RADIUS TEST SERVER ANSWERS AS IT SHOULD"
exit $fail
