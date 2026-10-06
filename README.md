# openxr-dispatcher-open

An open-source, drop-in replacement for the closed OpenXR dispatcher that
[OVRPort](https://github.com/ovrport/app) bundles into every converted Quest APK as
`lib/<abi>/libopenxr_loader.so`. [FramePort](https://github.com/spoopyghosty0/frameport)
uses OVRPort to run Quest titles on the Steam Frame, so this library also works there.

A Quest game loads `libopenxr_loader.so` expecting Meta's loader and a Quest runtime. The
dispatcher sits in that place: it loads the real OpenXR loader for the device (on the Steam
Frame, FramePort's FrameBridge in front of the Khronos loader and SteamVR), forwards every call
to it, and adds what Quest titles expect: a Quest runtime identity, the Quest extension list,
Meta reference spaces, binding and layer fixes, haptics emulation and local answers for
Meta-only functions. Every gap between what the game asks and what the runtime offers is logged
under the `OpenXRDispatch` logcat tag.

Chain on the Steam Frame:
game -> **this dispatcher** -> FrameBridge (FramePort) -> Khronos OpenXR loader -> SteamVR.

## Status

Version 1.0.0. Tested on the Steam Frame (SteamVR runtime via FramePort's FrameBridge) with
three OVRPlugin-based Quest titles: two Unreal Engine 4 games and one Unity game, all Vulkan,
at 90 Hz and 120 Hz. In each, the game ran with the same frame pacing, the same swapchains and
no new errors compared with OVRPort's closed dispatcher in the same APK. Other engines (Godot,
native OpenXR), GLES titles and non-Frame devices (Pico, YVR, Huawei loaders) follow the same
code paths but have not been tested on hardware; reports are welcome.

All 56 symbols exported by OVRPort's dispatcher are exported, plus `xrLocateSpaces`.

## Behaviour

What a game and the runtime observe. "Answered locally" means the runtime is not asked.

| Area | Behaviour |
|---|---|
| Next loader | `ro.product.brand` containing `pico`, `oculus`/`meta`, `yvr` or `huawei` (later match wins) selects `libopenxr_loader_<vendor>.so`; any other brand selects `libopenxr_loader_generic.so` (FrameBridge on the Steam Frame). Loaded by name, else from the dispatcher's own directory. The environment variable `OVERPORT_FORCE_LOADER_PATH` overrides the choice. A loader that resolves back to the dispatcher is refused and logged. |
| `xrGetInstanceProcAddr` | Always asks the next loader first. For the 124 functions in `src/functions.inc` the game gets the dispatcher's hook, else the runtime's function, else a stub returning `XR_ERROR_FUNCTION_UNSUPPORTED` (the lookup itself succeeds, as Quest titles expect). Any other name gets the runtime's answer unchanged. |
| Extension list | The 65 extensions a Quest reports (names and versions Quest titles check), plus every extension the runtime has. Size queries follow the spec (`XR_ERROR_SIZE_INSUFFICIENT`). |
| `xrCreateInstance` | Requested extensions the runtime lacks are not enabled and the game still gets `XR_SUCCESS`. `XR_EXT_local_floor` and `XR_BD_controller_interaction` are enabled when the runtime has them, `XR_FB_passthrough` with `force_passthrough`. `XrInstanceCreateInfoAndroidKHR` is chained when the game omits it (activity found through Unity or Unreal). An OpenXR 1.1 app on a 1.0 runtime is retried as 1.0. |
| Runtime identity | `xrGetInstanceProperties`: runtime name `Oculus`, version 67.522.0. |
| System identity | `xrGetSystemProperties`: system name `Oculus Quest2`, vendor id `0x2833`. Hand tracking, body tracking, passthrough and the virtual keyboard are reported as supported; calls into them fail with `XR_ERROR_FUNCTION_UNSUPPORTED` when the runtime lacks them. Space-warp motion-vector size 376x376. |
| `XR_META_headset_id` | All zeros, meaning "unknown headset". |
| Answered locally | Foveation profiles (`xrCreate/DestroyFoveationProfileFB`), swapchain state (`xrUpdateSwapchainFB`, `xrGetSwapchainStateFB`), `xrSetAndroidApplicationThreadKHR`, the `XR_META_performance_metrics` functions, `xrGetDeviceSampleRateFB` (500 Hz, the haptics emulation rate) and `xrGetRecommendedLayerResolutionMETA` (1504x1504, the Quest 2 default view size). Quest titles call these every frame or treat a failure as fatal, and they have no meaning on other runtimes. |
| Fallbacks | When the runtime fails or lacks them: `xrGetOpenGLESGraphicsRequirementsKHR` (GLES 3.0 to 3.2), the Vulkan instance/device extension lists, `xrEnumerateColorSpacesFB` (UNMANAGED), `xrSetColorSpaceFB` (success). `xrGetHandMeshFB` clamps invalid joint parents. |
| Events | `xrPollEvent` delivers one "headset mounted" event first; runtime errors become `XR_EVENT_UNAVAILABLE`. |
| Reference spaces | `LOCAL_FLOOR` and Meta's types 1000043000 to 1000043004 become native `LOCAL_FLOOR` (floor types) or `LOCAL` when the runtime has `XR_EXT_local_floor`; otherwise `LOCAL` lowered by the head height above the `STAGE` floor (1.7 m when that is implausible). `xrLocateSpace` treats time 0 as "now". |
| Views | Output structs are initialised; `suggested_texture_width/height` replace the recommended size when set. |
| Swapchains | Usage flags keep colour, depth-stencil and sampled bits (storage, transfer, mutable format and input attachment are dropped). Vulkan format numbers passed by GLES games are translated. Oversized swapchains and formats without a GLES equivalent go to the runtime unchanged; nothing aborts. |
| Frames | Known layer types (projection, quad, cube, cylinder, equirect, equirect2, FB and HTC passthrough) are kept, others dropped and logged. Layer chains keep colour scale/bias, image layout, alpha blend, secure content and layer settings; projection views keep only space-warp info (none with `disable_space_warp`). `force_passthrough` puts a passthrough layer first and alpha-blends the rest. Chain order is kept, NULL layers are skipped, a frame too large to rewrite is submitted unchanged. |
| Bindings | Paths a Touch controller lacks are removed (dpad, view, bumper, squeeze/menu touch, proximity); right-hand X/Y/menu move to the left hand. Bindings are suggested again for alias profiles (Pico 4/4S/Neo 3 for Touch, Steam Frame and Touch Plus games; Touch for Steam Frame games). `xrGetCurrentInteractionProfile` reports the game's own profile when the runtime picked an alias. Action spaces are passed through unchanged (no grip offset). |
| Haptics | Vibration amplitude and frequency are scaled by the multipliers. `XR_FB_haptic_pcm` and `XR_FB_haptic_amplitude_envelope` are emulated as 100 Hz amplitude samples played as 10 ms vibrations; the player thread runs only while a pattern plays and stops with its session. |
| Performance levels | `suggested_cpu_level` / `suggested_gpu_level` 1 to 4 replace the game's level; repeated requests are answered locally. |
| Network | None. |

## Settings

Read once, at the first OpenXR call. The names match OVRPort's, so existing configurations keep
working.

1. A JSON file `lib<configname>.so` (default `liboverport.config.so`; top-level keys, `//` and
   `/* */` comments allowed), from `/sdcard/Android/data/<package>/files/` first, else from the
   APK's native library directory.
2. Android system properties `debug.sys.ovrport.<property>`, then
   `persist.sys.ovrport.<property>` (a set `persist` value wins). They override the file, e.g.
   `adb shell setprop debug.sys.ovrport.verboselogging 1`.

| File key | Property | Default | Meaning |
|---|---|---|---|
| `suggested_texture_width` / `suggested_texture_height` | `fixedtexturewidth` / `fixedtextureheight` | 0 (runtime's size) | recommended eye-buffer size |
| `suggested_cpu_level` / `suggested_gpu_level` | `cpulevel` / `gpulevel` | 0 (game's choice) | 1 to 4 = power saving to boost |
| `vibration_multiplier` | `vibromultiplier` | 1.0 | haptic amplitude factor |
| `vibration_frequency_multiplier` | `vibrofreqmultiplier` | 1.0 | haptic frequency factor |
| `verbose_logging` | `verboselogging` | off | log first calls and failures of every hooked function, swapchain details |
| `disable_space_warp` | `disablespacewarp` | off | strip space-warp info from frames |
| `force_passthrough` | `forcepassthrough` | off | show passthrough behind the game |
| `disable_controller_offset` | `disablecontrolleroffset` | off | accepted for compatibility, unused |
| | `configname` | `overport.config` | base name of the settings file |

Flags accept `true`/`false` or numbers in the file and numbers as properties. Example file:

```json
{
  "version": 1,
  "vibration_multiplier": 0.5, // halve every vibration
  "disable_space_warp": true
}
```

## Logging

Tag `OpenXRDispatch` (`adb logcat -s OpenXRDispatch`):

- start-up: `open dispatcher 1.0.0: next loader <lib> (brand '<brand>')` and
  `settings from <source>: ...`;
- the runtime's extensions and the identity it replaces, `xrCreateInstance(<app>, N of M
  extensions)`, `xrCreateSession (GLES|Vulkan)`, reference-space mapping, one binding summary
  per interaction profile;
- `unsupported: ...` lines, each distinct message once: what the game asked for that the runtime
  does not provide. Functions the runtime lacks (and whether the dispatcher answers them or the
  calls fail), extensions not enabled, layer types dropped, bindings rejected, failing
  performance or haptics calls. These lines are the list of gaps to look at when a game
  misbehaves;
- with `verbose_logging`: the first call and every failure of each hooked function.

## Build and test

Android build (NDK r27c, API 29, `arm64-v8a` and `armeabi-v7a`; needs CMake and Ninja):

```sh
./build.sh                          # NDK from $NDK, else $ANDROID_NDK_HOME, else ~/sdk/android-ndk-r27c
NDK=/opt/android-ndk-r27c ./build.sh
tests/check_exports.sh prebuilt/arm64-v8a/libopenxr_loader.so
```

Output: `prebuilt/<abi>/libopenxr_loader.so`. `build.sh` also prints the imports and checks
that every export in `data/exports_overport.txt` is present.

Host tests (Linux x86_64, any C compiler as `cc`): the dispatcher is built for the host against a
fake `liblog`, loaded with a fake next loader (`tests/mock_next.c`) and driven the way OVRPlugin
drives it (`tests/test_host.c`).

```sh
tests/run.sh
SAN=1 tests/run.sh                  # with AddressSanitizer and UBSan
```

Source map: `src/functions.inc` (function table), `next.c` (next loader, `xrGetInstanceProcAddr`,
forwarding exports), `instance.c` (extensions, identity, events), `session.c` (sessions, spaces,
views, swapchains, performance), `frame.c` (`xrEndFrame`), `input.c` (bindings), `haptics.c`,
`emulated.c` (local answers), `formats.c`, `settings.c`, `log.c`.

## Install

1. Convert the game with OVRPort (or FramePort) as usual.
2. Replace `lib/<abi>/libopenxr_loader.so` in the converted APK with the file from
   `prebuilt/<abi>/` for each ABI the APK contains. Leave the other libraries (the vendor
   loaders, `libopenxr_loader_generic.so`, `liboverport.config.so`) in place.
3. Store the library uncompressed and page-aligned, then sign the APK with your own key, for
   example with the Android build-tools:

   ```sh
   mkdir -p lib/arm64-v8a && cp prebuilt/arm64-v8a/libopenxr_loader.so lib/arm64-v8a/
   zip -0 game.apk lib/arm64-v8a/libopenxr_loader.so
   zipalign -f -P 16 4 game.apk game-aligned.apk
   apksigner sign --ks your.keystore --out game-signed.apk game-aligned.apk
   ```

4. Install. To install over an existing build, sign with the same key that build used;
   otherwise uninstall it first.

## How the behaviour was derived

The behaviour was matched to OVRPort's closed dispatcher by static analysis of the shipped
binary and by observing it at runtime (logs and OpenXR calls of real games), then implemented
independently in this repository. No code or data was copied from it, except the
interoperability values games check: extension names and versions, the runtime name and
version, the vendor id and the default view size. Where the closed dispatcher crashes or
misbehaves (aborts on large swapchains or unknown formats, a wrong envelope length in haptics)
this implementation deliberately differs; the comments in `src/` say where.

## Licence

MIT, see `LICENSE`. Third-party files keep their licences:

- `vendor/openxr/`: Khronos OpenXR-SDK headers 1.1.63, `Apache-2.0 OR MIT`, Copyright The
  Khronos Group Inc. (`vendor/openxr/LICENSE`).
- `src/third_party/cJSON.*`: cJSON 1.7.18, MIT (`src/third_party/LICENSE-cJSON`).

OVRPort, FramePort and the games are separate projects with their own licences; this
repository contains none of their code.

## Contributing

- Game reports: open an issue with the game-report form. Include the engine, the headset and
  runtime, what happens, and the `adb logcat -s OpenXRDispatch` lines, in particular the
  `unsupported:` lines (and FrameBridge's, on the Steam Frame).
- Pull requests: keep behaviour changes covered by the host tests; `tests/run.sh` and
  `SAN=1 tests/run.sh` must pass.

See `CONTRIBUTING.md`.
