# nanostack-generic-standalone

The Nanostack network stack used by the Wi-SUN mesh emulator, extracted from
[`wisun-mbed-simulator`](../wisun-mbed-simulator) (commit `d86eb34`) into a
repository of its own. The emulator that consumes it lives in
[`mesh-emulator-orchestrator`](../mesh-emulator-orchestrator).

This first step is a pure move: the code is the same as in
`wisun-mbed-simulator`, file for file, and builds into the same machine code
(see [Verification](#verification)). Nothing was fixed or redesigned.

Status (2026-10-23):

- The stack already runs unchanged on FreeRTOS on the EFR32FG25. See
  [`nanostack-efr32fg25-freertos`](https://github.com/YannCharbon/nanostack-efr32fg25-freertos):
  two boards form a valid Wi-SUN network.
- Making the stack itself generic (porting API, POSIX port, in-process nodes)
  has not started. The plan and its progress table are in
  [`docs/GENERICITY_PLAN.md`](docs/GENERICITY_PLAN.md).
- This is the Silicon Labs-derived Wi-SUN stack (see [Licensing](#licensing)).
  It is not the code base for the fire-alarm protocol stack.

## Content

| Path | Was in `wisun-mbed-simulator` | Content |
| --- | --- | --- |
| `nanostack/` | `nanostack/` | 6LoWPAN, IPv6, RPL, MPL, Wi-SUN, security (host side) |
| `nanostack-event-loop/` | `nanostack-event-loop/` | Event loop and callback timers |
| `mbed-client-libservice/` | `mbed-client-libservice/` | `ns_list`, `ns_trace`, IPv6 string helpers |
| `rcp/mac/`, `rcp/fhss/`, `rcp/ccm/` | `app_wshwsim/mac/`, `fhss/`, `ccm/` | IEEE 802.15.4 MAC, FHSS and CCM, which run on the RCP side |
| `port/linux/common/` | `common/` (OS parts) | Linux platform layer: timers (timerfd), scheduler, critical sections, random, logging, lists |

`rcp/` keeps its own `mac/rf_driver_storage.h`, which differs slightly from
`nanostack/source/mac/rf_driver_storage.h`, exactly as before.

`port/linux/common/` keeps the `common/` directory name, so the stack's
`#include "common/rand.h"` style includes are unchanged. The Spinel/UART
transport (`bus_uart`, `spinel_buffer`) is emulator plumbing and stayed in the
orchestrator.

The git history of these files was carried over with `git filter-repo`
(paths renamed as in the table).

## Using it from CMake

The repository declares libraries only. Executables come from the consumer:

```cmake
add_subdirectory(path/to/nanostack-generic-standalone nanostack-build)

# Upper layers. CONFIG selects nanostack/source/configs/cfg_<config>.h.
nanostack_add_library(nanostack_ws_border_router CONFIG ws_border_router BORDER_ROUTER
                      DEFINITIONS EXTRA_DEBUG_INFO)
nanostack_add_library(nanostack_ws_router CONFIG ws_router)
# 802.15.4 MAC + FHSS + CCM, e.g. DEFINITIONS FHSS_CHANNEL_DEBUG
nanostack_add_rcp_library(nanostack_rcp)

target_link_libraries(my_app PRIVATE nanostack_ws_router)
```

Fixed targets:

- `nanostack_libservice`
- `nanostack_eventloop`
- `nanostack_port_linux`

The upper layers are built through functions because their compile-time
configuration (`NSCONFIG`, debug defines, border-router-only files) differs
per application. `BORDER_ROUTER` adds the files only the border router used:

- `ws_bbr_api.c`
- `ws_bootstrap_6lbr.c`
- `dhcpv6_server_service.c`
- `libdhcpv6_server.c`

Compile options (`-Wall`, `-funwind-tables`, build type) come from the
consumer's directory, as they did when the stack was part of the emulator's
CMake project.

## Building on its own

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<mbedtls v3.0.0 build dir> \
      -DCMAKE_C_FLAGS="-std=gnu17 -fpermissive"
ninja -C build
```

This builds the default variants listed above. It needs Threads and
MbedTLS v3.0.0, plus libbacktrace if it is installed.

`-std=gnu17 -fpermissive` is needed with GCC ≥ 14. GCC 15 defaults to C23 and
turns several legacy warnings into errors, so the inherited code doesn't build
without those flags (the same holds for `wisun-mbed-simulator`).

## Verification

The extraction was checked against `wisun-mbed-simulator` `d86eb34`, with the
same compiler and flags:

- **Object level:** the old build compiled stack sources into 317 objects,
  counted per executable. Every one of them is identical to its counterpart in
  the new build. That covers code with symbolic relocations and every
  non-debug data section. Of the 57 application objects in the orchestrator,
  50 are identical. The other 7 differ only through the absolute source path
  that `__FILE__` embeds.
- **Link level:** each new executable defines a subset of the old one's
  symbols. The static archives leave out objects nothing references, such as
  the authenticator security protocols in the router. Nothing else is missing
  or added.
- **Runtime:** `dag_simple` (15 nodes) runs were compared against the
  reference. See `mesh-emulator-orchestrator/MIGRATION.md`.

## Licensing

This code comes from Silicon Labs' `wisun-br-linux`. The top-level `LICENSE`
applies:

- Silicon Labs' modifications are under the Silicon Labs MSLA.
- The original Arm code is under Apache-2.0.

So this repository is **not yet free of MSLA code**. Making it so means
rebasing the stack on Arm's Apache-2.0 Nanostack. See
`documents/pre-study/legal/MSLA_License_Review.md` in `mesh-emulator-rework`.
