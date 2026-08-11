# syntax=docker/dockerfile:1

# Build stage: compile chat_client
FROM debian:bookworm-slim AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    ca-certificates \
    libsqlite3-dev \
    libsodium-dev \
    libncurses-dev \
    wget \
    curl \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy project sources
COPY . /src

# Configure and build only the chat_client target. Force C++23 at configuration time
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=23 \
  && cmake --build build --target chat_client -j"$(nproc)"

### Runtime image
FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    libsqlite3-0 \
    libsodium23 \
    libncurses6 \
    ca-certificates \
  && rm -rf /var/lib/apt/lists/*

# Copy the built client from the builder stage
COPY --from=build /src/build/chat_client /usr/local/bin/chat_client

RUN chmod +x /usr/local/bin/chat_client

ENTRYPOINT ["/usr/local/bin/chat_client"]
