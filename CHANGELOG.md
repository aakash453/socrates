# Edge AI Runtime — Change Log

## 0.1.0 (2026-07-18)

- Initial public scaffold.
- Core types: Result, strong IDs, tensor specs, fencing tokens.
- Build: CMake + Conan 2, C++20, presets for macOS/iOS/Android/Windows.
- Discovery: mDNS, UDP broadcast, Bluetooth + fallback coordinator.
- Cluster: authenticated membership, fenced Bully election.
- Scheduler: memory-based and sensitivity-aware shard placement.
- Pipeline: single/multi-stage inference with token streaming and replan.
- Runtime: lifecycle, daemon, stable C ABI bindings.
- Python: manifest validation, sharding, calibration, model fetch tools.
- Tests: 79 unit/contract/integration/E2E tests.
