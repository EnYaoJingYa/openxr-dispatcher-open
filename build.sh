#!/bin/sh
# SPDX-License-Identifier: MIT
# Build libopenxr_loader.so (the open dispatcher) for Android arm64-v8a and armeabi-v7a
# (API 29, the Quest minimum) with NDK r27c. The NDK is $NDK, else $ANDROID_NDK_HOME,
# else $HOME/sdk/android-ndk-r27c. Output: prebuilt/<abi>/libopenxr_loader.so.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ndk=${NDK:-${ANDROID_NDK_HOME:-"$HOME/sdk/android-ndk-r27c"}}
[ -f "$ndk/build/cmake/android.toolchain.cmake" ] || { echo "NDK not found at $ndk (set NDK)" >&2; exit 1; }
bin="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
for abi in arm64-v8a armeabi-v7a; do
  cmake -S "$here" -B "$here/build-android-$abi" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$abi -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release > /dev/null
  cmake --build "$here/build-android-$abi"
  mkdir -p "$here/prebuilt/$abi"
  so="$here/prebuilt/$abi/libopenxr_loader.so"
  cp "$here/build-android-$abi/libopenxr_loader.so" "$so"
  echo "== $abi: $(stat -c %s "$so") bytes"
  "$bin/llvm-readelf" -d "$so" | grep -E 'SONAME|NEEDED'
  echo "imports: $("$bin/llvm-nm" -D --undefined-only "$so" | awk '{print $2}' | sed 's/@.*//' | sort -u | tr '\n' ' ')"
  NM="$bin/llvm-nm" sh "$here/tests/check_exports.sh" "$so"
  sha256sum "$so"
done
