# Changelog

All notable changes to this project are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
