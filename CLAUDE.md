# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A Chinese-language **development notebook** for the ESP-01S (ESP8266) module. `README.md` is the
main deliverable: a step-by-step guide covering toolchain setup, environment variables, build/flash,
baud rates, boot-log analysis, partition tables, and TCP/WiFi control.

The code in `main/` **is a real application, not a stock example.** It began as the `hello_world`
example from `$IDF_PATH/examples/get-started/`, but has since grown into a remotely controlled
relay — `main.c` (relay driver + command parsing), `wifi_sta.c` (credentials in NVS, reconnect),
`tcp_client.c` (transport only; it knows nothing about lights). The notes exist to explain that code,
so keep prose and code in step when either changes.

The slave's **master is an ESP32**, and the link was chosen deliberately (README §9.2, decided
2026-09-25): the ESP32 runs a **SoftAP**, the slave is a **TCP client** to it, and no router is
involved. The deciding question was *which address has to be known*. The slave hardcodes
`TCP_SERVER_IP` in `tcp_client.c`, and the AP's `192.168.4.1` is a compile-time constant — assigned
unconditionally in `tcpip_adapter_init()` (`components/tcpip_adapter/tcpip_adapter_lwip.c:207-209`),
not DHCP. The slave's own address *is* DHCP-assigned, but nobody needs to know it, because the slave
dials out. Rejected: both nodes on a router, which would make the *master's* address DHCP and break
the hardcoded line. Deferred, not rejected: ESP-NOW — `tcp_client.c` is transport-only (`send` +
`set_rx_handler`) so it can be replaced without touching `main.c`.

The two ends are compiled **independently** — the ESP32 side is ESP-IDF, a different project, not
this SDK — so four values must match by hand: SSID (`WIFI_SSID_DEFAULT`), password
(`WIFI_PASSWORD_DEFAULT`), port (`TCP_SERVER_PORT`), and **authmode**. `wifi_sta.c` sets
`threshold.authmode = WIFI_AUTH_WPA2_PSK`, so the slave *rejects* an open SoftAP; the ESP32's
`ap.authmode` must be WPA2-PSK with a ≥8-byte password. `192.168.4.1` was verified only on the
ESP8266 side — there is no ESP-IDF source in this tree to cross-check it against, so read it off the
ESP32's boot log rather than asserting it.

**There is no provisioning fallback.** SmartConfig used to be `wifi_sta.c`'s answer to "the node
can't reach its AP": the phone broadcast the credentials in special 802.11 frames and the node
sniffed them. It was **removed** (see README §9.7), because the chain — phone WiFi driver → AP →
promiscuous mode → the closed-source library — has four links we don't control and fails silently
when any one of them does. A 60-second air probe measured 17090 frames and **0 broadcast data
frames** on a busy channel with ~35 beacons/s, which is physically impossible (ARP/DHCP/mDNS/IPv6-ND
are all multicast), proving the probe was blind on exactly the axis it needed. Do not re-add it.
The accepted cost: once the node is off the air, the only recovery is reflashing — so the documented
order is always ① push `wifi <SSID>,<pass>` over TCP, ② then change the master node.

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

## Doxygen comments

All three sources in `main/` are commented in Doxygen style, matching the SDK's own house style
(`/**` with a two-space `*` indent, `@brief` / `@param[in]` / `@retval` / `@note` / `@warning` — see
any function in `components/esp8266/include/esp_wifi.h`). Keep new comments in that style; a
half-converted file is worse than an unconverted one.

`Doxyfile` at the repo root renders them, and it is **not** a copy of the SDK's `docs/Doxyfile` —
that one is set to `GENERATE_HTML = NO` / `GENERATE_XML = YES` because the ESP-IDF docs pipeline eats
XML, so copying it yields no browsable site. The load-bearing settings here:

| Setting | Value | Why it matters |
| --- | --- | --- |
| `INPUT` | `main` | the default is **empty**, which searches only the current directory — and there are no sources at the root |
| `EXTRACT_STATIC` | `YES` | **the default is `NO`**; most functions in `wifi_sta.c` and `main.c` are `static`, so without this they vanish from the output |
| `GENERATE_LATEX` | `NO` | needs a TeX install to be useful, and nothing here uses it |
| `RECURSIVE` | `NO` (default) | fine because `main/` is flat — revisit if a subdirectory is ever added |

`html/` and `latex/` are gitignored (`.gitignore:11` and `:12`); `Doxyfile` itself is meant to be
committed. Doxygen lives at `D:\doxygen\bin\doxygen.exe` (winget id `DimitriVanHeesch.Doxygen`) and
is **not** on the `PATH` of every shell here.

**Angle brackets inside a Doxygen comment are parsed as HTML.** Writing `wifi <SSID>,<密码>` yields
`warning: Unsupported xml/html tag <SSID> found` and renders wrong — escape them as `\<SSID\>`. This
applies to the comments **only**: the same text inside a `printf()` string (e.g. the format hint in
`cmd_do_wifi()`) must be left alone, because escaping it would change what the firmware prints.

The C compiler accepts all of this silently — it treats Doxygen markup as ordinary comments — so
`build/verify_src.py` passing says nothing about whether the comments are well-formed. The only real
check is a clean Doxygen run: `doxygen Doxyfile 2>&1 | grep -i warning` should print nothing.
`WARN_IF_UNDOCUMENTED` is left at its default `YES`, so a newly added undocumented struct or enum
will show up there rather than pass unnoticed.

## VSCode extension caveats

`.vscode/` is **not tracked** (`.gitignore:7` is `/.vscode`), and is generated by the third-party
`dzantemir.esp8266-idf` extension (the official Espressif extension covers ESP32 only). Known sharp
edges:

- `.vscode/tasks.json` hardcodes absolute paths, including the extension version:
  `c:\Users\Administrator\.vscode\extensions\dzantemir.esp8266-idf-1.93.6\python\launcher.py`.
  Upgrading the extension, or moving to another machine, breaks every task. Regenerate rather than
  hand-edit.
- `.vscode/c_cpp_properties.json` has a **stale** `includePath` pointing at `g:\github\esp-01S\...`,
  a layout that no longer exists (the real one is `g:\github\esp8266_toochain\msys32\home\Administrator\...`).
  IntelliSense will not resolve SDK headers until this is corrected. Its `compilerPath` also points into
  `C:\Users\Administrator\.espressif\tools\...`, a third toolchain copy distinct from the one the
  `make` flow uses.
- **Ignore rules here work; don't chase ghosts.** `git ls-files` shows only `sdkconfig` tracked.
  `.vscode/` and `sdkconfig.old` are ignored (`.gitignore:7` and `:8`) and genuinely untracked —
  a previous version of this file claimed they were already tracked and that the rules had no
  effect. That was wrong. Note `sdkconfig` itself is *deliberately* tracked and *not* ignored:
  `.gitignore:8` is `/sdkconfig.old`, not `/sdkconfig`.
