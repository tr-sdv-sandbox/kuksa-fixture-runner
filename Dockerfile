# Build stage - install dependencies and build libkuksa-cpp + fixture-runner
FROM debian:bookworm-slim AS builder

# Install build tools and dependencies
RUN apt-get update && apt-get install -y \
    g++ \
    cmake \
    make \
    git \
    ca-certificates \
    libssl-dev \
    libprotobuf-dev \
    protobuf-compiler \
    protobuf-compiler-grpc \
    libgrpc++-dev \
    libgrpc-dev \
    libgoogle-glog-dev \
    libyaml-cpp-dev \
    nlohmann-json3-dev \
    libgtest-dev \
    pkg-config \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Clone and build libkuksa-cpp
WORKDIR /tmp/libkuksa-cpp
RUN git clone --depth 1 https://github.com/tr-sdv-sandbox/libkuksa-cpp.git . && \
    mkdir -p build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DBUILD_EXAMPLES=OFF \
          -DBUILD_TESTS=OFF \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          .. && \
    make -j$(nproc) && \
    make install && \
    cd / && rm -rf /tmp/libkuksa-cpp

# Copy fixture runner source
COPY . /app/
WORKDIR /app

# Build the application
RUN rm -rf build && mkdir -p build && cd build && \
    cmake .. && \
    make -j$(nproc) && \
    strip --strip-all fixture-runner

# Runtime stage - minimal runtime image
FROM debian:bookworm-slim

# Install only runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libprotobuf32 \
    libgrpc++1.51 \
    libgoogle-glog0v6 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && rm -rf /usr/share/doc/* \
    && rm -rf /usr/share/man/* \
    && rm -rf /usr/share/locale/* \
    && find /var/log -type f -delete

# Copy the stripped binary from builder
COPY --from=builder /app/build/fixture-runner /usr/local/bin/

# Create non-root user
RUN useradd -m -u 1000 -s /bin/false appuser

# Set working directory
WORKDIR /app
RUN chown appuser:appuser /app

# Switch to non-root user
USER appuser

# Set default environment variables
ENV KUKSA_ADDRESS=databroker \
    KUKSA_PORT=55555

# Run the fixture runner
CMD ["fixture-runner"]
