#!/usr/bin/env bash
# The remote console's end-to-end encryption: the firmware's code (mbedTLS,
# firmware/src/c2_e2e.cpp) against WebCrypto as the admin page uses it (Node).
#   tests/c2/e2e_test.sh     (needs docker and node 20+)
set -euo pipefail
cd "$(dirname "$0")/../.."
docker build -q -t rs232c2-fwhost-env tests/c2 >/dev/null
B=$(mktemp -d); trap 'rm -rf $B' EXIT
docker run --rm -v "$PWD":/src:ro -v "$B":/build -w /src rs232c2-fwhost-env \
  g++ -std=c++17 -O1 -Wall -Wextra -Ifirmware/src tests/c2/e2e_host.cpp firmware/src/c2_e2e.cpp -lmbedcrypto -o /build/e2e_host
# the host binary runs in the same image (its mbedTLS)
cat > $B/run <<'X'
#!/bin/sh
exec docker run --rm -i -v "$(dirname "$0")":/build rs232c2-fwhost-env /build/e2e_host "$@"
X
chmod +x $B/run
node tests/c2/e2e_test.mjs $B/run
