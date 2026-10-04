# Changelog

All notable changes to this project are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Prometheus exporter payload serving `/metrics` on port 9100.
- Temperatures (CPU, SoC sensors), CPU frequency, uptime, boot time, model
  and system version, memory, filesystems, network counters, process count.
- ShadowMountPlus integration over its local JSON API: version, storage
  overview, game list with mounted/installed/source state.
- `/health` endpoint and a start-up notification with the port.
