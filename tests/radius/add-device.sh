#!/usr/bin/env bash
# One more client certificate from the test authority, for another device:
#   ./add-device.sh rs232-second     -> data/device/rs232-second.p12 (.pem), password printed and noted in data/README.txt
# The RADIUS server needs nothing for it: it accepts every certificate of its authority.
set -euo pipefail
cd "$(dirname "$0")"
N=${1:?name of the device, as it will appear in the RADIUS log}
D=data
[ -e $D/ca.key ] || { echo "run ./setup.sh first"; exit 1; }
[ ! -e $D/device/$N.p12 ] || { echo "data/device/$N.p12 exists already"; exit 1; }
T=$(mktemp -d); trap 'rm -rf $T' EXIT
openssl req -newkey rsa:2048 -nodes -keyout $T/k -out $T/csr -subj "/O=RS232 Console Test/CN=$N" 2>/dev/null
printf 'basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=clientAuth,serverAuth\nsubjectAltName=DNS:rs232,DNS:rs232.local\n' > $T/ext
openssl x509 -req -in $T/csr -CA $D/certs/ca.pem -CAkey $D/ca.key -CAserial $D/ca.srl -CAcreateserial -days 825 -sha256 -extfile $T/ext -out $T/crt 2>/dev/null
cat $T/crt $T/k > $D/device/$N.pem; chmod 600 $D/device/$N.pem
P=$(openssl rand -hex 6)
openssl pkcs12 -export -in $T/crt -inkey $T/k -certfile $D/certs/ca.pem -name "$N" -passout pass:$P -out $D/device/$N.p12 2>/dev/null
chmod 600 $D/device/$N.p12
printf '\nDevice %s, EAP-TLS (added %s)\n  client certificate device/%s.p12  (password %s)  or  device/%s.pem\n' "$N" "$(date +%F)" "$N" "$P" "$N" >> $D/README.txt
echo "data/device/$N.p12  (password $P)"
