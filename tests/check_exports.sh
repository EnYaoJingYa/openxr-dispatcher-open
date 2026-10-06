#!/bin/sh
# SPDX-License-Identifier: MIT
# Every export of OVRPort's dispatcher (data/exports_overport.txt) must be exported by the build.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
so=${1:-$here/prebuilt/arm64-v8a/libopenxr_loader.so}
nm=${NM:-nm}
have=$($nm -D --defined-only "$so" | awk '{print $3}' | sort -u)
want=$here/data/exports_overport.txt
miss=$(echo "$have" | comm -13 - "$want")
extra=$(echo "$have" | comm -23 - "$want" | tr '\n' ' ')
echo "exports: $(echo "$have" | wc -l) ($(wc -l < "$want") of OVRPort's, missing: ${miss:-none}; extra: ${extra:-none})"
[ -z "$miss" ]
