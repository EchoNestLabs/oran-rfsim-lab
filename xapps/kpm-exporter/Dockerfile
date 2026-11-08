# SPDX-License-Identifier: Apache-2.0
#
# kpm-exporter sample container: same build path as kpm-hello - builds the
# xApp against the pinned FlexRIC v3.0.0 source tree inside the lab's SDK
# image, registering one new example directory at build time.
#
# Build:  docker compose -f docker-compose.yml -f docker-compose.samples.yml \
#           build kpm-exporter
ARG FLEXRIC_SRC_TAG=oran/flexric-src:v1
FROM ${FLEXRIC_SRC_TAG}

COPY kpm_exporter.c /tmp/kpm-exporter/kpm_exporter.c
COPY CMakeLists.txt /tmp/kpm-exporter/CMakeLists.txt

RUN mkdir -p /src/flexric/examples/xApp/c/exporter \
 && cp /tmp/kpm-exporter/kpm_exporter.c /tmp/kpm-exporter/CMakeLists.txt \
       /src/flexric/examples/xApp/c/exporter/ \
 && grep -q 'add_subdirectory(monitor)' /src/flexric/examples/xApp/c/CMakeLists.txt \
 && sed -i 's|add_subdirectory(monitor)|add_subdirectory(exporter)\nadd_subdirectory(monitor)|' \
       /src/flexric/examples/xApp/c/CMakeLists.txt \
 && cd /src/flexric/build \
 && cmake .. -DCMAKE_BUILD_TYPE=Release -DASN1C_EXEC_PATH=/opt/asn1c/bin/asn1c \
 && make -j"$(nproc)" xapp_kpm_exporter \
 && rm -rf /tmp/kpm-exporter

# stdbuf: docker logs is not a TTY; keep startup lines unbuffered
ENTRYPOINT ["stdbuf", "-o0", \
            "/src/flexric/build/examples/xApp/c/exporter/xapp_kpm_exporter"]
