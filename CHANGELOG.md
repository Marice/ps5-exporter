# Changelog

All notable changes to this project are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
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
