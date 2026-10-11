# syntax=docker/dockerfile:1
# Reproducible build of the occt-wasm module. Source of truth for releases.
#
#   docker buildx build -f docker/Dockerfile --target dist --output type=local,dest=dist .
#
# Stages:
#   occt-libs  static OCCT libraries (slow, cached by OCCT sources + flags)
#   facade     compiles the facade and links the module
#   dist       exports dist/ contents only

# emscripten/emsdk:6.0.12 (multi-arch index digest, see versions.json)
ARG EMSDK_IMAGE=emscripten/emsdk:6.0.12@sha256:77638e6c215a65f25a704c0c831d5c2463a8c0468a58c6cb82489dbe621dfa05

FROM ${EMSDK_IMAGE} AS base
WORKDIR /work

FROM base AS occt-libs
# Only what the OCCT build depends on, so facade changes keep this layer cached.
COPY scripts/config-occt.sh scripts/build-occt.sh /work/scripts/
COPY third_party/occt /work/third_party/occt
RUN /work/scripts/build-occt.sh /work/third_party/occt /tmp/occt-build /opt/occt \
    && rm -rf /tmp/occt-build

FROM base AS facade
COPY --from=occt-libs /opt/occt /opt/occt
COPY scripts /work/scripts
COPY facade /work/facade
COPY js /work/js
COPY LICENSE OCCT_LGPL_EXCEPTION.txt NOTICE versions.json /work/
RUN /work/scripts/build-facade.sh /opt/occt /tmp/facade-build /work/dist

FROM scratch AS dist
COPY --from=facade /work/dist/ /
