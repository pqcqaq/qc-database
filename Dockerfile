# Multi-stage build for Distributed Cache Server
FROM ubuntu:22.04 AS builder

# Switch to Chinese mirror for faster downloads
RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    sed -i 's/security.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libprotobuf-dev \
    protobuf-compiler \
    libgrpc++-dev \
    libgrpc-dev \
    libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# Copy source code
COPY . .

# Build the project
RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)

# Production image
FROM ubuntu:22.04

# Switch to Chinese mirror for faster downloads
RUN sed -i 's/archive.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list && \
    sed -i 's/security.ubuntu.com/mirrors.aliyun.com/g' /etc/apt/sources.list
    
# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    libprotobuf-dev \
    libgrpc++1 \
    libgrpc10 \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Create cache user
RUN groupadd -r cache && useradd -r -g cache cache

# Create directories
RUN mkdir -p /opt/cache/bin /opt/cache/config /opt/cache/data /opt/cache/logs && \
    chown -R cache:cache /opt/cache

# Copy binary and configuration
COPY --from=builder /build/build/cache_server /opt/cache/bin/
COPY --from=builder /build/config/cache_config.yaml /opt/cache/config/
COPY --from=builder /build/scripts/ /opt/cache/scripts/

# Make scripts executable
RUN chmod +x /opt/cache/scripts/*.sh

# Set working directory
WORKDIR /opt/cache

# Switch to cache user
USER cache

# Expose ports
EXPOSE 8080 9090

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

# Default command
CMD ["/opt/cache/bin/cache_server", "--config", "/opt/cache/config/cache_config.yaml"]