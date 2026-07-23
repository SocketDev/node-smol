# syntax=docker/dockerfile:1
#
# node-smol-base — the heavy Node.js/V8 compile environment, FROM the
# wheelhouse c-base (ghcr.io/socketdev/c-base). Repo-owned: the recipe + the
# bake workflow (.github/workflows/node-smol-base-publish.yml) live here;
# the shared parent layers (base → c-base) stay wheelhouse-owned. Split into
# its own layer for CACHE LOCALITY: the toolchain rarely changes, the Node
# source changes every version bump, so isolating them keeps c-base warm for
# the small parts. The .mts driver's node comes via
# `COPY --from=ghcr.io/socketdev/node-base`, not a runtime baked here.
ARG FLEET_BASE=ghcr.io/socketdev/c-base:latest
FROM ${FLEET_BASE}

ENV DEBIAN_FRONTEND=noninteractive

# V8 wants a specific python; ccache speeds the repeated V8 rebuilds.
RUN apt-get -o Acquire::Check-Valid-Until=false update \
    && apt-get install -y --no-install-recommends \
      python3.12 \
      python3.12-venv \
    && rm -rf /var/lib/apt/lists/* /var/cache/apt/* \
    && rm -rf /usr/share/doc /usr/share/man /usr/share/info \
    && update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.12 100

ENV CCACHE_MAXSIZE=10G
LABEL dev.socket.fleet.base="node-smol-base"
