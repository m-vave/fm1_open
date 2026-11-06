# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
#
# Runtime for JieLi's Linux PI32v2 toolchain (clang 4.0.1, GNU ld 2.26). The
# toolchain is x86-64 only, so the image is always linux/amd64 (Rosetta on
# Apple Silicon). The toolchain itself is NOT baked in: build.sh mounts the
# verified, extracted release read-only at /opt/jieli, and the source tree
# read-only at /src, so the image carries no third-party binaries.
#
# The pinned base was measured to run every toolchain binary with no extra
# packages (no ldd "not found"), so nothing is installed.
FROM --platform=linux/amd64 debian:bookworm-slim@sha256:99c0b25c46d0214d5a2091092e7908907dcaecd6a962cde0ac700acee222b6f3

LABEL org.opencontainers.image.title="fm1-open-build" \
      org.opencontainers.image.description="Runtime for the JieLi Linux PI32v2 toolchain (mounted at /opt/jieli)"

ENV LC_ALL=C HOME=/tmp
WORKDIR /out
CMD ["/bin/bash", "/opt/fm1/container-build.sh"]
