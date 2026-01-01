# Multi-stage build for minimal image size
FROM gcc:13-bookworm AS builder

WORKDIR /build

# Copy source files
COPY src/ ./src/
COPY include/ ./include/
COPY Makefile ./

# Build with Docker-portable flags
RUN make docker

# Runtime image - minimal Debian
FROM debian:bookworm-slim

WORKDIR /app

# Install minimal runtime dependencies
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Copy binaries from builder
COPY --from=builder /build/dsm-server /usr/local/bin/dsm-server
COPY --from=builder /build/dsm-client /usr/local/bin/dsm-client

# Create non-root user
RUN useradd -m -u 1000 dsm && \
    chown -R dsm:dsm /app

USER dsm

# Expose default port
EXPOSE 9999

# Default to server mode
ENTRYPOINT ["/usr/local/bin/dsm-server"]
CMD []
