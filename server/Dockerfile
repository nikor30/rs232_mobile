# Build:  docker build -t rs232-c2 .
# Tests:  docker build --target test .
FROM golang:1.25-alpine AS build
WORKDIR /src
COPY go.mod ./
COPY cmd ./cmd
COPY internal ./internal
RUN --mount=type=cache,target=/root/.cache/go-build \
    CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/ ./cmd/... && mkdir /data

FROM build AS test
RUN --mount=type=cache,target=/root/.cache/go-build \
    test -z "$(gofmt -l .)" && go vet ./... && go test -count=1 ./...

# No shell, no package manager: the two static binaries and nothing else.
FROM scratch
COPY --from=build /out/c2server /out/devsim /
# An empty /data owned by the service user: a fresh volume mounted there
# inherits that ownership, so the server can write its state.
COPY --from=build --chown=10001:10001 /data /data
# numeric user: there is no /etc/passwd in this image
USER 10001:10001
VOLUME /data
EXPOSE 8443
ENTRYPOINT ["/c2server"]
