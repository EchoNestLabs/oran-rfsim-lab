# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Builds xapps/kpm-exporter INSIDE the pinned FlexRIC source tree (see the
# Dockerfile): the include paths are relative to /src/flexric, same as the
# upstream examples in examples/xApp/c/.
add_executable(xapp_kpm_exporter
  kpm_exporter.c
  ../../../../src/util/alg_ds/alg/defer.c
  )

target_link_libraries(xapp_kpm_exporter
                      PUBLIC
                      e42_xapp
                      -pthread
                      -lsctp
                      -ldl
                      )
