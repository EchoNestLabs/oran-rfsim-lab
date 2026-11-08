# Builds Open5GS from the pinned upstream tag (no prebuilt image exists).
# One image, all NF binaries; each compose service runs one of them.
# v2.8.0 exposes the JSON info endpoints (/gnb-info, /ue-info, /pdu-info)
# consumed by the open5gs-nms monitoring tier; 2.7.x does not.
FROM ubuntu:22.04

ARG OG5GS_REF=v2.8.0

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      python3-pip python3-setuptools python3-wheel ninja-build build-essential \
      flex bison git cmake meson \
      libsctp-dev libc-ares-dev libgnutls28-dev libgcrypt-dev libssl-dev \
      libmongoc-dev libbson-dev libyaml-dev libnghttp2-dev libmicrohttpd-dev \
      libcurl4-gnutls-dev libtins-dev libtalloc-dev libidn11-dev \
      iproute2 curl && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN git clone --depth 1 --branch ${OG5GS_REF} https://github.com/open5gs/open5gs.git && \
    cd open5gs && \
    meson build --prefix=/usr && \
    ninja -C build && \
    ninja -C build install && \
    rm -rf /src/open5gs
