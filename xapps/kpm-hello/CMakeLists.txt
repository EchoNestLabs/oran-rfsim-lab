# SPDX-License-Identifier: Apache-2.0
#
# Builds xapps/kpm-hello INSIDE the pinned FlexRIC source tree (see the
# Dockerfile): the include paths are relative to /src/flexric, same as the
# upstream examples in examples/xApp/c/.
add_executable(xapp_kpm_hello
  kpm_hello.c
  ../../../../src/util/alg_ds/alg/defer.c
  )

target_link_libraries(xapp_kpm_hello
                      PUBLIC
                      e42_xapp
                      -pthread
                      -lsctp
                      -ldl
                      )
