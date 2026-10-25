#!/bin/sh
# SPDX-License-Identifier: MIT
# Host test: build the dispatcher for x86_64 Linux against a fake liblog, load it with a
# fake next loader (tests/mock_next.c) and run tests/test_host.c. SAN=1 adds ASan/UBSan.
set -eu
san=""; [ "${SAN:-0}" = 1 ] && san="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=$here/build-host
rm -rf "$out"; mkdir -p "$out"
inc="-I$here/vendor -I$here/src -I$here/tests"
cc -shared -fPIC -O1 -o "$out/liblog.so" "$here/tests/fake_liblog.c"
cc $san -shared -fPIC -O2 -std=gnu11 -fvisibility=hidden -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format-truncation $inc \
   -Wl,-soname,libopenxr_loader.so -Wl,-Bsymbolic $([ -z "$san" ] && echo -Wl,--no-undefined) \
   -o "$out/libopenxr_loader.so" "$here"/src/*.c "$here/src/third_party/cJSON.c" -L"$out" -llog -lpthread -ldl -lm
cc $san -shared -fPIC -O1 -std=gnu11 -fvisibility=hidden -Wall $inc -o "$out/libmock_next.so" "$here/tests/mock_next.c"
cc $san -O1 -std=gnu11 -Wall $inc -o "$out/test_host" "$here/tests/test_host.c" -L"$out" -llog -ldl -lpthread -lm
store=$(mktemp -d)
mkdir -p "$store/data/test_pkg/files"
cat > "$store/data/test_pkg/files/liboverport.config.so" <<'JSON'
{ "version": 1,
  /* OVRPort's parser accepts comments */
  "vibration_multiplier": 0.5, // halves every vibration
  "disable_space_warp": false,
  "database": [] }
JSON
OVERPORT_FORCE_LOADER_PATH="$out/libmock_next.so" XD_PACKAGE=test_pkg XD_STORAGE="$store" \
XD_PROP_ro_product_brand=Lenovo XD_PROP_ro_product_model=VRX \
XD_PROP_debug_sys_ovrport_fixedtexturewidth=1440 XD_PROP_debug_sys_ovrport_cpulevel=2 \
XD_PROP_persist_sys_ovrport_cpulevel=3 LD_LIBRARY_PATH="$out" \
  "$out/test_host" "$out/libopenxr_loader.so" "$out/libmock_next.so"
rm -rf "$store"
