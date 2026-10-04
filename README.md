# ps5-exporter

A Prometheus exporter for a jailbroken PS5. It runs as a payload next to
your other payloads and serves `GET /metrics` on port 9100, so Prometheus
(or anything that speaks its text format) can graph your console.

What it reports:

- CPU and SoC temperatures and the CPU frequency
- uptime and boot time, model and system software version
- memory, filesystems (size and free space per mount), network bytes per
  interface, number of processes
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

## Metrics

| Metric | Labels | Meaning |
|---|---|---|
| `ps5_temperature_celsius` | `sensor` (cpu, soc0..3) | Temperatures the console reports |
| `ps5_cpu_frequency_hertz` | | CPU frequency |
| `ps5_uptime_seconds`, `ps5_boot_time_seconds` | | Uptime and boot time |
| `ps5_info` | `model`, `system_version` | Console model and firmware |
| `ps5_memory_physical_bytes`, `ps5_memory_user_bytes`, `ps5_memory_free_bytes` | | Memory |
| `ps5_filesystem_size_bytes`, `ps5_filesystem_avail_bytes` | `mountpoint`, `fstype` | Mounted filesystems |
| `ps5_network_receive_bytes_total`, `ps5_network_transmit_bytes_total` | `device` | Interface counters |
| `ps5_processes` | | Number of processes |
| `ps5_shadowmount_up`, `ps5_shadowmount_info` | `version` | ShadowMountPlus reachable and its version |
| `ps5_shadowmount_storage_*_bytes` | `mount_point`, `source`, `filesystem` | Storage as ShadowMount sees it |
| `ps5_shadowmount_games` | | Number of managed games |
| `ps5_shadowmount_game_info` | `title_id`, `name`, `platform`, `source_type` | Game metadata |
| `ps5_shadowmount_game_mounted` | `title_id` | 1 while the game's runtime mount is active |
| `ps5_shadowmount_game_installed`, `ps5_shadowmount_game_source_available` | `title_id` | Registration and source state |
| `ps5_exporter_scrapes_total`, `ps5_exporter_build_info` | | Exporter itself |

A useful Grafana panel: `ps5_temperature_celsius{sensor="cpu"}` against
`max(ps5_shadowmount_game_mounted) by (title_id)` to see which game heats
the console up.

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

## Not yet

CPU and GPU load, FPS and fan speed. etaHEN shows these in its overlay, but
they come from kernel memory with firmware-specific offsets. The fan duty
(`sceKernelGetCurrentFanDuty`) exists in `libkernel_sys`; it will be added
once it is verified from a payload.

## License

GPL-3.0-or-later. See `LICENSE`.
