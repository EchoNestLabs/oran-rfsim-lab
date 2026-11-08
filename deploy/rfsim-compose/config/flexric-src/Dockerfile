# FlexRIC SDK image: pinned v3.0.0 source + asn1c + full build.
# Provides the near-RT RIC and the example xApp binaries (xapp_kpm_moni) and
# keeps the matching e42 libs under /usr/local/lib/flexric. Built once by
# up.sh (config: this file); the released oai-flexric image has an
# incompatible E42 wire format - do not mix.
FROM oaisoftwarealliance/oai-flexric:v3.0.0

# flexric rejects gcc-11 (Ubuntu 22.04 default); pin gcc-12 explicitly.
ENV CC=/usr/bin/gcc-12 \
    CXX=/usr/bin/g++-12

RUN apt-get update && apt-get install -y --no-install-recommends \
      git cmake gcc-12 g++-12 make autoconf automake libtool m4 bison flex \
      libsctp-dev libpcre2-dev python3 curl ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 120 --slave /usr/bin/g++ g++ /usr/bin/g++-12

# ASN.1 compiler for RRC/F1AP generation — same commit as flexric scripts/install_asn1c.sh
ENV PATH=/opt/asn1c/bin:$PATH
RUN git clone --depth 1 https://github.com/mouse07410/asn1c /tmp/asn1c \
    && cd /tmp/asn1c && git fetch --depth 1 origin 940dd5fa9f3917913fd487b13dfddfacd0ded06e \
    && git checkout 940dd5fa9f3917913fd487b13dfddfacd0ded06e \
    && autoreconf -iv && ./configure --prefix=/opt/asn1c \
    && make -j"$(nproc)" && make install && rm -rf /tmp/asn1c

RUN git clone --depth 1 --branch v3.0.0 https://github.com/duranta-project/flexric /src/flexric

# Build the RIC + xApp examples and install, so /usr/local/lib/flexric .so match
# the binaries exactly (mixing with the base image's .so segfaults: ABI drift
# between v3.0.0 commits).
RUN cd /src/flexric && mkdir -p build && cd build \
    && cmake -DASN1C_EXEC_PATH=/opt/asn1c/bin/asn1c .. -DCMAKE_BUILD_TYPE=Release \
    && make -j"$(nproc)" \
    && make install
