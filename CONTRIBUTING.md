# Contributing

## Reporting a game

Use the "Game report" issue form. The most useful part is the log:

```sh
adb logcat -c
# start the game, reproduce the problem
adb logcat -d -s OpenXRDispatch > dispatch.log
```

Attach the relevant lines, especially every `unsupported:` line. On the Steam Frame, add
FrameBridge's `unsupported:` lines too.

Before posting a log, remove personal data: home directory paths, user and account names,
IP addresses, Steam IDs, device serial numbers. Do not attach game files, APKs or OBBs.

## Pull requests

- One topic per pull request; describe the observable behaviour that changes.
- Add or update a check in `tests/test_host.c` for the behaviour you change.
- `tests/run.sh` and `SAN=1 tests/run.sh` must pass; `./build.sh` must build both ABIs
  without warnings (the build uses `-Werror`).
- New files carry an `SPDX-License-Identifier: MIT` line. By contributing you agree to license
  your contribution under the MIT License.
- Do not submit code or data taken from closed binaries.
