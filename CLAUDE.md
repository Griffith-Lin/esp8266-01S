# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A Chinese-language **development notebook** for the ESP-01S (ESP8266) module. `README.md` is the
main deliverable: a step-by-step guide covering toolchain setup, environment variables, build/flash,
baud rates, boot-log analysis, partition tables, and TCP/WiFi provisioning.

The code in `main/` **is a real application, not a stock example.** It began as the `hello_world`
example from `$IDF_PATH/examples/get-started/`, but has since grown into a remotely controlled
relay — `main.c` (relay driver + command parsing), `wifi_sta.c` (credentials in NVS, reconnect,
SmartConfig fallback), `tcp_client.c` (transport only; it knows nothing about lights). The notes
exist to explain that code, so keep prose and code in step when either changes.

An OTA module (`ota.c`/`ota.h`) used to live in `main/`. It was **moved out** to
`../ota-for-larger-flash/` (a sibling of this repo under `project/`) because it cannot run on a
1MB board — two slots don't fit. Do not move it back or re-add `ACT_OTA` without a larger module
and a two-slot partition table. Its directory has its own README with the return procedure.

`README.md` is written in Chinese in an explanatory teaching style — tables, blockquotes,
"为什么" asides, tree diagrams, word-split mnemonic breakdowns. Match that voice when editing.
The table of contents at the top is manual; update it when adding or renaming a section.

## The environment is the hard part

The build environment lives **outside** this repo, under `G:\github\esp8266_toochain\`:

| Path | What it is |
| --- | --- |
| `xtensa-lx106-elf/` | Xtensa LX106 cross-compiler (the `make` flow uses this one) |
| `msys32/mingw32.exe` | MSYS2 shell — the only shell that can build this project |
| `msys32/home/Administrator/esp/ESP8266_RTOS_SDK/` | the SDK (`v3.4-115-g858c7c2e`) |

This repo deliberately sits *inside* the msys32 tree, because the SDK build system rejects paths
containing spaces or non-ASCII characters.

Constraints responsible for most build failures:

- **`mingw32.exe` does not inherit Windows environment variables.** `IDF_PATH` and `PATH` must be
  re-exported in every new shell session (README §2). Running `make` from Git Bash, PowerShell, or a
  Claude Code Bash tool will fail — those shells have no toolchain on `PATH`.
- **Python must be the 32-bit install** (`Python313-32`), not the 64-bit one.
- **`setuptools` must stay pinned `<71`.** v71+ removed `pkg_resources`, which the SDK Makefiles
  import; installing the latest silently breaks the build.
- **`partitions_1mb.csv` must stay pure ASCII.** `gen_esp32part.py` decodes it as ASCII and aborts on
  any non-ASCII byte — *including inside comments*, which are decoded before the `#` check. A trailing
  comment after the last comma is also fatal: that column is the `flags` field, and anything other than
  `encrypted` raises `unknown flag`. Keep notes on their own `#` lines, in ASCII. Because this file is
  read during CMake *configure*, a bad byte breaks every target, not just the build.

The SDK is frozen at v3.4 (final ESP8266_RTOS_SDK release). Do not suggest upgrading it or porting
this project to ESP-IDF — ESP-IDF v4+ dropped ESP8266 entirely.

## Build and flash

Run from `mingw32.exe`, after the §2 exports:

```bash
make menuconfig                          # the serial port is set here, as //./COM8
make -j10                                # build
make ESPPORT=COM8 ESPBAUD=921600 flash   # flash
make monitor                             # serial monitor; exit with Ctrl+]
make clean                               # when the build goes strange
```

There is no test suite and no lint step.

Hardware state matters more than the software:

- **Download mode**: GPIO0 tied to GND. **Run mode**: GPIO0 floating (the board has a 10k pullup).
- **Monitor baud is 74880**, not 115200 — that is the ESP8266 ROM bootloader's fixed output rate, and
  anything else turns the `ets Jan 8 2013,...` boot header into garbage. Flash baud (921600) is a
  separate setting.
- This module is the **1MB** flash variant. Verify with `esptool.py --port COM8 flash_id` — never with
  the boot log. `CONFIG_SPI_FLASH_SIZE` is **never probed from the chip**; it is copied into
  `g_rom_flashchip.chip_size` (spi_flash.c:67-74) and echoed back by both `spi_flash_get_chip_size()`
  (spi_flash.c:790) *and* the `SPI Flash Size :` boot line (bootloader_init.c:281, read from the image
  header esptool wrote at flash time). `spi_flash_erase_sector()`'s bounds check (spi_flash.c:465)
  compares that same config value against itself, so **a wrong size does not fail — it aliases**:
  1MB silicon decodes only A0–A19, so `0x108000 & 0xFFFFF = 0x008000`, and an erase past the end
  silently wraps to the start and destroys the partition table, nvs, and the running app. This has
  already bricked this board once.

## Two frontends, one SDK

`Makefile` and `CMakeLists.txt` both exist and both are valid — they are the two supported frontends
shipped with ESP8266_RTOS_SDK v3.4. `README.md` documents the `make` flow; the VSCode extension drives
the CMake flow. Both consume the same SDK:

- `Makefile` → `$(IDF_PATH)/make/project.mk`
- `CMakeLists.txt` → `$ENV{IDF_PATH}/tools/cmake/project.cmake`

Keep both entry points working when adding source files. Only `main/CMakeLists.txt` needs the new
file added to `SRCS` — `main/component.mk` is the SDK's default stub, which compiles every `.c` in
the directory automatically and so needs no edit.

## VSCode extension caveats

`.vscode/` is committed, and is generated by the third-party `dzantemir.esp8266-idf` extension (the
official Espressif extension covers ESP32 only). Known sharp edges:

- `.vscode/tasks.json` hardcodes absolute paths, including the extension version:
  `c:\Users\Administrator\.vscode\extensions\dzantemir.esp8266-idf-1.93.6\python\launcher.py`.
  Upgrading the extension, or moving to another machine, breaks every task. Regenerate rather than
  hand-edit.
- `.vscode/c_cpp_properties.json` has a **stale** `includePath` pointing at `g:\github\esp-01S\...`,
  a layout that no longer exists (the real one is `g:\github\esp8266_toochain\msys32\home\Administrator\...`).
  IntelliSense will not resolve SDK headers until this is corrected. Its `compilerPath` also points into
  `C:\Users\Administrator\.espressif\tools\...`, a third toolchain copy distinct from the one the
  `make` flow uses.
- `.vscode/*.json` and `sdkconfig.old` are matched by `.gitignore` but are **already tracked**, so the
  ignore rules have no effect on them — edits show up in `git status`.
