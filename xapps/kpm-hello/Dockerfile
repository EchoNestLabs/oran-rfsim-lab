# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# kpm-hello sample container: builds the xApp against the pinned FlexRIC
# v3.0.0 source tree that ships inside the lab's SDK image. The sample
# source is a simplified derivative of FlexRIC's xapp_kpm_moni example
# (CSSL v1.0, see ../LICENSES/LicenseRef-CSSL-1.0.txt). Nothing in the
# SDK image itself is modified - the sample adds one new example directory
# at build time (examples/xApp/c/hello) the same way upstream examples are
# registered.
#
# Build:  docker compose -f docker-compose.yml -f docker-compose.samples.yml \
#           build kpm-hello
ARG FLEXRIC_SRC_TAG=oran/flexric-src:v1
FROM ${FLEXRIC_SRC_TAG}

COPY kpm_hello.c /tmp/kpm-hello/kpm_hello.c
COPY CMakeLists.txt /tmp/kpm-hello/CMakeLists.txt

RUN mkdir -p /src/flexric/examples/xApp/c/hello \
 && cp /tmp/kpm-hello/kpm_hello.c /tmp/kpm-hello/CMakeLists.txt \
       /src/flexric/examples/xApp/c/hello/ \
 && grep -q 'add_subdirectory(monitor)' /src/flexric/examples/xApp/c/CMakeLists.txt \
 && sed -i 's|add_subdirectory(monitor)|add_subdirectory(hello)\nadd_subdirectory(monitor)|' \
       /src/flexric/examples/xApp/c/CMakeLists.txt \
 && cd /src/flexric/build \
 && cmake .. -DCMAKE_BUILD_TYPE=Release -DASN1C_EXEC_PATH=/opt/asn1c/bin/asn1c \
 && make -j"$(nproc)" xapp_kpm_hello \
 && rm -rf /tmp/kpm-hello

# stdbuf: docker logs is not a TTY; keep the periodic reports unbuffered
ENTRYPOINT ["stdbuf", "-o0", \
            "/src/flexric/build/examples/xApp/c/hello/xapp_kpm_hello"]
