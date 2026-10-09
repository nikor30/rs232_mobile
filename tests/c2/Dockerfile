# Build environment for the host test: mbedTLS 2.28, the major version the
# ESP32 Arduino core 2.0.17 ships, so the firmware's crypto code compiles and
# runs unchanged.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends g++ gcc libmbedtls-dev && rm -rf /var/lib/apt/lists/*
