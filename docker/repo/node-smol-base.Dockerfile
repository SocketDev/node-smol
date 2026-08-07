# syntax=docker/dockerfile:1
#
# node-smol/base — the heavy Node.js/V8 compile environment, FROM the wheelhouse
# c-base (ghcr.io/socketdev/socket-wheelhouse/c-base). Repo-owned: the recipe +
# the bake workflow (.github/workflows/prebake-publish.yml) live here; the shared
# parent layers (base → c-base) stay wheelhouse-owned. Split into its own layer
# for CACHE LOCALITY: the toolchain rarely changes, the Node source changes every
# version bump, so isolating them keeps c-base warm for the small native parts.
# The .mts driver's node comes via
# `COPY --from=ghcr.io/socketdev/socket-wheelhouse/node-base`, not a runtime
# baked here.
# Digest resolved from c-base:latest on 2026-08-07 via
# `docker buildx imagetools inspect`; refresh the pin when the wheelhouse
# republishes c-base.
ARG FLEET_BASE=ghcr.io/socketdev/socket-wheelhouse/c-base:latest@sha256:a625cbaa4212cdde97207b4bc11395cbd6359e8800bdf30acda30a4cccd46d8f
FROM ${FLEET_BASE}

ENV DEBIAN_FRONTEND=noninteractive

# V8 wants a specific python; ccache speeds the repeated V8 rebuilds (the ccache
# binary + CCACHE_DIR come from base). c-base already carries the C/C++
# toolchain, libicu-dev, zlib1g-dev, and ld.gold the V8 link needs.
RUN apt-get -o Acquire::Check-Valid-Until=false update \
    && apt-get install -y --no-install-recommends \
      python3.12 \
      python3.12-venv \
    && rm -rf /var/lib/apt/lists/* /var/cache/apt/* \
    && rm -rf /usr/share/doc /usr/share/man /usr/share/info \
    && update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.12 100

ENV CCACHE_MAXSIZE=10G

LABEL dev.socket.fleet.base="node-smol/base"
# Links the pushed package to this repo so GHCR inherits its access + visibility:
# node-smol's bake workflow gets push access with no manual per-package grant,
# and the one-time public flip sticks. See the wheelhouse docker-prebakes doc,
# "Registry setup (one-time)".
LABEL org.opencontainers.image.source="https://github.com/SocketDev/node-smol"
