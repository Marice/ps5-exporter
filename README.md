# ps5-exporter

A Prometheus exporter for a jailbroken PS5. It runs as a payload next to
your other payloads and serves `GET /metrics` on port 9100, so Prometheus
(or anything that speaks its text format) can graph your console.

What it reports:

- CPU and SoC temperatures (all sensors the kernel answers for; 17 on a
  CFI-7121), fan speed and the CPU frequency
- the direct memory pool (games and GPU) and its largest free block
- uptime and boot time, model and system software version
- filesystems (size and free space per mount)
- the process table: number of processes, CPU time, resident memory and
  instance count per executable name (so Grafana can show what eats CPU)
- from ShadowMountPlus, when it runs: its version, storage overview, the
  game list with per-title `mounted` (1 while a game runs), `installed` and
  `source_available`

Everything is read-only. The port is open to your LAN like the FTP and web
services of the other payloads are; do not forward it to the internet.

## Installing

Download `ps5-exporter.elf` from the releases page and load it like any
other payload:

- **Payload Manager**: upload it in the web UI and add it to the autoload
  list so it starts with every jailbreak, or
- **elfldr**: `nc -w 5 <ps5-ip> 9021 < ps5-exporter.elf`.

A notification confirms the port when it starts. Check with:

```sh
curl http://<ps5-ip>:9100/metrics
```

Another port: pass it as an argument or as `EXPORTER_PORT` in the
environment (websrv: `/elfldr?elf=/data/pldmgr/payloads/ps5-exporter/ps5-exporter.elf&env=EXPORTER_PORT%3D9101`).
`GET /quit` stops the exporter so a newer build can take the port without a
reboot; it changes nothing else.

## Prometheus

```yaml
scrape_configs:
  - job_name: ps5
    scrape_interval: 30s
    static_configs:
      - targets: ["192.168.68.125:9100"]
```

When the console is off or in rest mode the scrape fails and Prometheus
marks the target down; the series simply have gaps.

## Grafana

`grafana/ps5-exporter.json` is a ready-made dashboard: console on/off, CPU
gauge, uptime, firmware and model, what is playing, temperature history
with the running games drawn underneath, storage usage, the ShadowMount
game library as a table, CPU frequency, processes and the exporter itself.
Import it in Grafana (Dashboards, New, Import), pick your Prometheus data
source, and select the scrape job (`ps5` by default).

## Metrics

| Metric | Labels | Meaning |
|---|---|---|
| `ps5_temperature_celsius` | `sensor` (cpu, soc0..socN) | Temperatures the console reports; every SoC sensor index that answers |
| `ps5_cpu_frequency_hertz` | | CPU frequency |
| `ps5_fan_duty_ratio` | | Fan duty cycle, 0 to 1 |
| `ps5_cpu_usage_ratio` | `core` | CPU usage per core, 0 to 1. Use `avg()` for the overall load |
| `ps5_cpu_cores` | | Cores the kernel reports usage for |
| `ps5_soc_power_watts`, `ps5_soc_power_raw_value` | | SoC power draw (the unit of the raw value is unconfirmed) |
| `ps5_power_operating_seconds_total`, `ps5_power_cycles_total` | | Hours powered on and power cycles since the console was new |
| `ps5_uptime_seconds`, `ps5_boot_time_seconds` | | Uptime and boot time |
| `ps5_info` | `model`, `firmware`, `system_version` | Model, the kernel's firmware version (`13.60`) and the version the system API reports |
| `ps5_filesystem_size_bytes`, `ps5_filesystem_avail_bytes`, `ps5_filesystem_free_bytes` | `mountpoint`, `fstype` | Mounted filesystems (`free` includes reserved blocks, so used = size - free) |
| `ps5_processes` | | Number of processes |
| `ps5_process_cpu_seconds_total`, `ps5_process_resident_bytes`, `ps5_process_instances` | `name` | CPU time, resident memory and count per executable name |
| `ps5_direct_memory_bytes`, `ps5_direct_memory_largest_free_bytes` | | Direct memory pool and its largest free block |
| `ps5_shadowmount_up`, `ps5_shadowmount_info` | `version` | ShadowMountPlus reachable and its version |
| `ps5_shadowmount_storage_*_bytes` | `mount_point`, `source`, `filesystem` | Storage as ShadowMount sees it |
| `ps5_shadowmount_games` | | Number of managed games |
| `ps5_shadowmount_game_info` | `title_id`, `name`, `platform`, `source_type` | Game metadata |
| `ps5_shadowmount_game_mounted` | `title_id` | 1 while the game's runtime mount is active |
| `ps5_shadowmount_game_installed`, `ps5_shadowmount_game_source_available` | `title_id` | Registration and source state |
| `ps5_shadowmount_games_cache_age_seconds` | | Age of the cached game list |
| `ps5_exporter_scrapes_total`, `ps5_exporter_build_info` | | Exporter itself |
| `ps5_exporter_collector_success` | `collector` | 1 when that collector produced data in the last scrape |

Process metrics are aggregated per executable name rather than per pid: a
pid label would create a new time series for every process the console ever
starts, and `rate()` cannot work on series that live for a few seconds. The
trade-off is that the counter drops when a process exits, which Prometheus
reads as a counter reset.

A useful Grafana panel: `ps5_temperature_celsius{sensor="cpu"}` against
`max(ps5_shadowmount_game_mounted) by (title_id)` to see which game heats
the console up.

## Behaviour under failure

- A scrape has a four second budget for the ShadowMount calls, so it always
  fits inside Prometheus' default ten second timeout. The game list is
  cached for five minutes (it rarely changes) and served from cache in
  between; `ps5_shadowmount_games_cache_age_seconds` tells you how fresh it
  is.
- Metrics that the console refuses are left out rather than guessed, and
  `ps5_exporter_collector_success` shows which collector produced nothing.
- If the whole page does not fit in the output buffer the exporter answers
  HTTP 500 instead of serving half a scrape, so Prometheus records an error
  rather than silently losing metrics.
- When the listening socket becomes unusable, which is what suspending and
  resuming the console can do, the exporter binds a fresh one instead of
  spinning on a dead socket.
- `/quit` only answers on the loopback address: stopping the exporter is a
  state change, so it is not reachable from the LAN.

## Tests

`make test` builds the host tests with AddressSanitizer and UBSan and runs
them. They cover the append buffer, the sensor range checks and the JSON
parser, including a game list far larger than the output buffer, which is
how the overflow this code once had was found. No console needed.

## Building

Needs the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk).

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make             # ps5-exporter.elf
make upload      # copy it to /data/pldmgr/payloads/ps5-exporter/ over FTP (port 1337)
```

No libraries beyond the SDK: a small HTTP/1.0 server and client on BSD
sockets, `sysctl`, `getmntinfo` and `getifaddrs` for the system numbers,
and the libkernel temperature calls the SDK's `hwinfo` sample uses.

## What works on a console, and what does not

Verified on a CFI-7121 running firmware 13.60, as a payload under the
Payload Manager autoloader:

| Metric | Status |
| --- | --- |
| Temperatures (17 sensors), CPU frequency, uptime, model, firmware | works |
| Fan speed (`ps5_fan_duty_ratio`) | works |
| Filesystems, direct memory, ShadowMount storage and games | works |
| Per-process CPU and memory | works, see the record size note below |
| Per-core CPU usage, SoC power, lifetime counters | the console refuses these calls from a payload; the metrics are left out and `ps5_exporter_collector_success` reports 0 |

The process table records are 1096 bytes on this firmware while the SDK
header describes 1088, so the exporter takes the record size from the
kernel itself and publishes it as `ps5_process_record_bytes`. If a future
firmware changes the layout again, that metric shows it instead of the
details silently disappearing.

## How the hardware values are read

Fan, per-core CPU usage, SoC power and the lifetime counters come from
libkernel functions that are not in the SDK headers. Their signatures come
from two homebrew projects that call them on real consoles:
[drakmor/fan_target](https://github.com/drakmor/fan_target) (the
ShadowMountPlus author) for `sceKernelGetCurrentFanDuty(uint16_t*, uint64_t*)`
with its 0..1024 scale, and
[aloksaurabh/elf-arsenal](https://github.com/aloksaurabh/elf-arsenal) for
`sceKernelGetCpuUsageAll(int*, int*)` and `sceKernelGetSocPowerConsumption`.

These are ordinary library calls, not kernel memory reads at fixed offsets,
so they are not tied to one firmware version. Every value is range-checked
before it is published: a console that refuses a call, or answers something
implausible, simply leaves that metric out.

## Not yet

- GPU load and FPS. etaHEN shows these in its overlay, but they come from
  kernel memory with firmware-specific offsets, which would tie the exporter
  to one firmware.
- Memory: the `hw.physmem` and `hw.usermem` sysctls are refused for
  payloads on 13.60.
- Network counters: `getifaddrs` works, but the `if_data` layout in the SDK
  headers does not match what the kernel returns (the numbers come out far
  too small). The code is there behind `-DEXPORTER_NETWORK` for whoever
  wants to pin down the layout.

`firmware` comes from the kernel (`kernel_get_fw_version`, 0x13600007 is
13.60). `system_version` is what `sceKernelGetSystemSwVersion` reports to
applications; a jailbreak chain can spoof that lower (13.590.001 here) to
keep update prompts away, so the two can differ.

## License

GPL-3.0-or-later. See `LICENSE`.
