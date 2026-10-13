#!/usr/bin/env bash
# Makes everything the test RADIUS server needs, once, into data/ (which git
# ignores): its own certificate authority, the server's certificate, a client
# certificate for the device, a user for PEAP/TTLS and the secret the access
# points share with the server. Run again only to start over: --force.
set -euo pipefail
cd "$(dirname "$0")"
LAN=${RADIUS_LAN:-192.168.10.0/24}       # where the access points are
D=data

if [ -e $D/secret ] && [ "${1:-}" != "--force" ]; then
  echo "data/ exists already - nothing changed (start over with: $0 --force)"; exit 0
fi
rm -rf $D; mkdir -p $D/certs $D/device; chmod 700 $D
q() { openssl "$@" 2>/dev/null; }

# certificate authority, ten years
q req -x509 -newkey rsa:2048 -nodes -days 3650 -keyout $D/ca.key -out $D/certs/ca.pem \
  -subj "/O=RS232 Console Test/CN=RS232 Test RADIUS CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign"

sign() {                                 # name, subject, extensions
  q req -newkey rsa:2048 -nodes -keyout $D/$1.key -out $D/$1.csr -subj "$2"
  printf '%s\n' "$3" > $D/$1.ext
  q x509 -req -in $D/$1.csr -CA $D/certs/ca.pem -CAkey $D/ca.key -CAserial $D/ca.srl -CAcreateserial -days 825 -sha256 -extfile $D/$1.ext -out $D/$1.crt
  rm $D/$1.csr $D/$1.ext
}
sign server "/O=RS232 Console Test/CN=radius.rs232.test" \
  $'basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:radius.rs232.test'
# for the device: clientAuth for 802.1X, serverAuth and names so that it can serve its web interface with it too
sign device "/O=RS232 Console Test/CN=rs232-console" \
  $'basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=clientAuth,serverAuth\nsubjectAltName=DNS:rs232,DNS:rs232.local'

cat $D/server.crt $D/server.key > $D/certs/server.pem
rm $D/server.crt $D/server.key
chmod 644 $D/certs/*                     # the server in the container reads them as another user

# what goes onto the device
cp $D/certs/ca.pem $D/device/radius-ca.pem
cat $D/device.crt $D/device.key > $D/device/rs232-console.pem; chmod 600 $D/device/rs232-console.pem
P12=$(openssl rand -hex 6)
q pkcs12 -export -in $D/device.crt -inkey $D/device.key -certfile $D/certs/ca.pem -name rs232-console -passout pass:$P12 -out $D/device/rs232-console.p12
rm $D/device.crt $D/device.key

SECRET=$(openssl rand -hex 12)
PASS=$(openssl rand -base64 12 | tr -dc 'A-Za-z0-9' | cut -c1-14)
echo "$SECRET" > $D/secret

cat > $D/clients.conf <<CONF
# written by setup.sh
client localhost {
	ipaddr = 127.0.0.1
	secret = $SECRET
}
client lan {
	ipaddr = $LAN
	secret = $SECRET
}
# the test in a container next to the server, and whatever Docker shows as the sender
client docker {
	ipaddr = 172.16.0.0/12
	secret = $SECRET
}
CONF
cat > $D/authorize <<CONF
# written by setup.sh - the user for PEAP-MSCHAPv2 and EAP-TTLS
rs232	Cleartext-Password := "$PASS"
CONF
chmod 644 $D/clients.conf $D/authorize

cat > $D/README.txt <<TXT
Test RADIUS server - what to enter where (made $(date +%F))

Access point / controller
  RADIUS server      this computer's address, port 1812 (accounting 1813), UDP
  shared secret      $SECRET

Device, EAP-TLS
  client certificate device/rs232-console.p12  (password $P12)  or  device/rs232-console.pem
  CA certificate     device/radius-ca.pem
  identity           empty (the device takes "rs232-console" from the certificate)

Device, PEAP-MSCHAPv2 or EAP-TTLS
  user               rs232
  password           $PASS
  CA certificate     device/radius-ca.pem (recommended)
TXT
chmod 600 $D/README.txt $D/secret $D/ca.key
cat $D/README.txt
