# Changelog

All notable changes to this project are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [v0.2.0] - 2026-10-06

### Fixed
- Buffer overflow when ShadowMount reported more games than fitted in the
  output buffer: the append helpers counted what they would have written
  instead of what they did, so the length ran past the capacity and the
  next collector wrote outside the buffer. All output now goes through one
  append-only buffer that cannot overflow, confirmed by a test with 900
  games under AddressSanitizer.
- Per-core CPU usage printed the wrong value because the core index was
  missing from the format arguments.
- A slow client could hold the single-threaded server indefinitely: reading
  a request and writing a response now have wall-clock deadlines.
- An unusable listening socket (which suspending and resuming the console
  can cause) made the accept loop spin forever instead of rebinding.
- JSON parsing read past the end of the response on truncated escapes and
  short `true`/`false` values.
- Per-process metrics were missing on firmware 13.60: its process records
  are 1096 bytes while the SDK header describes 1088, so the record size
  now comes from the kernel instead of the header.

### Changed
- `ps5_fan_duty_percent` and `ps5_cpu_usage_percent` are now
  `ps5_fan_duty_ratio` and `ps5_cpu_usage_ratio` (0 to 1, the Prometheus
  convention). `ps5_fan_duty_raw` is gone, `ps5_soc_power_raw` is now
  `ps5_soc_power_raw_value`.
- `ps5_cpu_usage_ratio` no longer carries a `core="all"` sample: an
  aggregate inside the same family makes `sum()` double-count. Use `avg()`.
- Process metrics are aggregated per executable name instead of per pid,
  which keeps the number of time series bounded. Adds
  `ps5_process_instances`.
- The ShadowMount game list is cached for five minutes and all its calls
  share a four second budget, so a scrape stays inside Prometheus' timeout.
- `/quit` only answers on loopback.
- The version now comes from the Makefile, so the binary and the release
  tag cannot disagree.

### Added
- Dashboard: the game library table now joins the per-title state onto the
  info metric, so names, platform and source show up; panels for metrics
  firmware 13.60 refuses say so instead of "No data".
- `ps5_filesystem_free_bytes`, `ps5_exporter_collector_success`,
  `ps5_shadowmount_games_cache_age_seconds`, `ps5_process_record_bytes`,
  `ps5_process_instances`.
- `make test`: host tests with sanitizers, no console needed.

### Added
- Fan speed (`ps5_fan_duty_percent`, raw 0..1024 scale from the kernel).
- Per-core CPU usage and the average (`ps5_cpu_usage_percent{core}`).
- SoC power draw and the lifetime counters: hours powered on and power
  cycles since new.
- Dashboard: SoC power and lifetime panels; the fan and CPU panels now have
  data.
- All SoC temperature sensors the kernel answers for (not only 0..3).
- Direct memory pool size and largest free block.
- Per-process CPU time and resident memory from the process table, checked
  against `ki_structsize` so a different kernel layout only drops them.
- Dashboard: fan, CPU usage, top processes by CPU and by memory, direct
  memory panels (fan and CPU usage fill in once the exporter reports them).
- `make probe`: development build with a `/probe/<name>` endpoint for
  working out undocumented kernel calls on hardware (not in releases).

## [v0.1.1] - 2026-10-04

### Added
- Grafana dashboard (`grafana/ps5-exporter.json`).
- `ps5_info` gets a `firmware` label from the kernel version; `system_version`
  (the spoofable user-space value) stays alongside it.

## [v0.1.0] - 2026-10-04

### Added
- Prometheus exporter payload serving `/metrics` on port 9100.
- Temperatures (CPU, SoC sensors), CPU frequency, uptime, boot time, model
  and system version, filesystems, process count.
- ShadowMountPlus integration over its local JSON API: version, storage
  overview, game list with mounted/installed/source state.
- `/health` and `/quit` endpoints, a start-up notification with the port,
  and the port configurable by argument or `EXPORTER_PORT`.
