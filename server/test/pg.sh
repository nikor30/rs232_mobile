#!/usr/bin/env bash
# The Go tests once more, this time with the state in a real PostgreSQL: starts
# a throw-away database server and runs the tests against it.
#
#   ./test/pg.sh [go test arguments, default ./...]     (needs docker)
set -euo pipefail
cd "$(dirname "$0")/.."
P=rs232c2-pg

cleanup() { docker rm -fv $P-db >/dev/null 2>&1 || true; docker network rm $P >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup

docker network create $P >/dev/null
docker run -d --name $P-db --network $P -e POSTGRES_PASSWORD=test -e POSTGRES_DB=c2 --tmpfs /var/lib/postgresql/data \
       postgres:17-alpine -c fsync=off >/dev/null
for i in $(seq 1 60); do docker exec $P-db pg_isready -q -U postgres -d c2 2>/dev/null && break; sleep 1; done

docker run --rm --network $P -v "$PWD":/src:ro -w /src -v rs232c2-gomod:/go/pkg/mod -v rs232c2-gobuild:/root/.cache/go-build \
       -e C2_TEST_DATABASE_URL="postgres://postgres:test@$P-db:5432/c2?sslmode=disable" \
       golang:1.25-alpine go test -count=1 "${@:-./...}"
