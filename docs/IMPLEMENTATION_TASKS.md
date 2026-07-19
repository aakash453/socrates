# Edge AI Runtime Implementation Tasks

**Design baseline:** Authoritative runtime-owned tasks extracted from blueprint v1.0.

Each task is scoped to one agent session. Tasks in the same parallel group may run completely in parallel only after their listed dependencies are complete. Implement only the referenced files and matching tests. Read `ENGINE_BLUEPRINT.md` and `CODING_CONVENTIONS.md` before starting.

Dependencies retain the original combined-plan task IDs. App-owned task `8.8` and mixed cross-repository release orchestration task `8.10` are intentionally not runtime implementation tasks.

## Phase 1: Core Interfaces and Build Foundation

| ID | Task | Files | Dependencies | Parallel group |
|---|---|---|---|---|
| 1.1 | Create CMake/Conan targets, options, warnings, and test target skeleton. | `CMakeLists.txt`, `cmake/*`, `conanfile.py`, `CMakePresets.json` | None | A |
| 1.2 | Add common result, IDs, tensor, capability, and fence types. | `include/socrates/result.h`, `include/socrates/types.h`, tests | None | A |
| 1.3 | Add Protobuf modules and generation configuration for the canonical schemas. | `proto/socrates/v1/model_manifest.proto`, `proto/socrates/v1/control_plane.proto`, `proto/socrates/v1/data_plane.proto`, `proto/socrates/v1/trace.proto`, `buf.yaml`, `buf.gen.yaml` | None | A |
| 1.4 | Scaffold Python package, lint/type/test configuration. | `pyproject.toml`, `python/socrates_engine/*` | None | A |
| 1.5 | Scaffold runtime logging/tracing context conventions without subsystem logic. | `configs/logging.example.json`, logging wrapper files | 1.1, 1.2 | B |
| 1.6 | Implement manifest golden/malformed fixtures and cross-language schema tests. | `tests/contract/manifests/*`, `tests/contract/protobuf/*` | 1.3 | B |

## Phase 2: Discovery and Cluster Lifecycle

| ID | Task | Files | Dependencies | Parallel group |
|---|---|---|---|---|
| 2.0 | Implement local identity generation/storage and ephemeral, pinned, and private-CA trust adapters. | `src/security/*`, identity tests | 1.1, 1.2 | C |
| 2.1 | Implement mDNS discovery adapter and unit tests. | `src/discovery/mdns_discovery.cpp`, tests | 1.1, 1.2 | C |
| 2.2 | Implement bounded UDP broadcast discovery adapter and tests. | `src/discovery/udp_discovery.cpp`, tests | 1.1, 1.2 | C |
| 2.3 | Implement platform Bluetooth discovery adapters and permission-error tests. | `src/discovery/bluetooth_discovery.cpp`, platform files, tests | 1.1, 1.2 | C |
| 2.4 | Implement fallback-order discovery coordinator. | `include/socrates/discovery/discovery_service.h`, discovery coordinator source/tests | 2.1–2.3 | — |
| 2.5 | Implement authenticated membership state and snapshot revisions using a fake transport. | `src/cluster/membership_service.cpp`, tests | 1.2, 2.0, 2.4 | — |
| 2.6 | Implement fenced Bully election with an in-memory `ElectionTermStore` fake. | `src/cluster/bully_election.cpp`, tests | 1.2, 2.5 | — |
| 2.7 | Add join/leave/leader-loss cluster integration tests. | `tests/integration/cluster_*` | 2.5, 2.6 | — |

## Phase 3: Capability Profiling

| ID | Task | Files | Dependencies | Parallel group |
|---|---|---|---|---|
| 3.1 | Implement Android CPU/RAM/GPU/NPU/QNN inspection. | `src/profiler/platform/android_profiler.cpp`, tests | 1.2 | D |
| 3.2 | Implement Apple CPU/RAM/GPU/Neural Engine inspection. | `src/profiler/platform/apple_profiler.mm`, tests | 1.2 | D |
| 3.3 | Implement Windows CPU/RAM/GPU/NPU inspection. | `src/profiler/platform/windows_profiler.cpp`, tests | 1.2 | D |
| 3.4 | Implement Linux/container inspection for E2E nodes. | `src/profiler/platform/linux_profiler.cpp`, tests | 1.2 | D |
| 3.5 | Implement synthetic benchmark orchestration against an injected synthetic-backend port. | `src/profiler/capability_profiler.cpp`, tests | 3.1–3.4 | — |
| 3.6 | Implement RTT/bandwidth measurement policy against an injected transport-probe fake. | profiler network source/tests | 1.2 | D |
| 3.7 | Integrate profile publication/expiry into membership. | membership/profiler integration tests | 2.5, 3.5, 3.6 | — |

## Phase 4: Transport

| ID | Task | Files | Dependencies | Parallel group |
|---|---|---|---|---|
| 4.1 | Implement gRPC unary control-plane adapter with mTLS/deadlines/limits. | `src/transport/grpc_transport.cpp`, `proto/socrates/v1/control_plane.proto`, tests | 1.1–1.3, 2.0 | E |
| 4.2 | Implement tensor envelope codec, shape/size/SHA validation. | `src/transport/tensor_codec.cpp`, `proto/socrates/v1/data_plane.proto`, tests | 1.2, 1.3 | E |
| 4.3 | Implement ordered bidirectional activation stream and backpressure. | `src/transport/grpc_transport.cpp`, tests | 4.1, 4.2 | — |
| 4.4 | Implement idempotent broadcast and bounded request deduplication. | transport source/tests | 4.1 | E |
| 4.5 | Add loss, delay, reorder, timeout, and cancellation integration tests. | `tests/integration/transport_*` | 4.3, 4.4 | — |

## Phase 5: Model, Scheduler, and Persistence

| ID | Task | Files | Dependencies | Parallel group |
|---|---|---|---|---|
| 5.1 | Implement transactional SQLite assignment store and migrations. | `src/persistence/sqlite_assignment_store.cpp`, tests | 1.1, 1.2 | F |
| 5.2a | Implement envelope parsing, unknown-field rejection, and manifest SHA validation. | `src/model/manifest_repository.cpp`, parser tests | 1.3, 1.6 | F |
| 5.2b | Implement graph, range, boundary, ID, and tensor semantic validation. | manifest validator source/tests | 5.2a | — |
| 5.2c | Implement artifact, quantization, execution-profile, fallback, and calibration validation/indexing. | manifest validator/index tests | 5.2a | F |
| 5.3 | Implement local-only model artifact resolver, SHA verification, and leases. | `src/model/model_manager.cpp`, tests | 5.2b, 5.2c | — |
| 5.4 | Implement deterministic memory-based contiguous sharding. | `src/scheduler/memory_scheduler.cpp`, tests | 1.2, 5.2b, 5.2c | G |
| 5.5 | Implement deterministic sensitivity/interleaving scheduler. | `src/scheduler/sensitivity_scheduler.cpp`, tests | 1.2, 5.2b, 5.2c | G |
| 5.6 | Add pipeline balancing and calibration cost refinement to both policies. | scheduler sources/tests | 5.4, 5.5 | — |
| 5.7 | Add scheduler property tests for coverage, memory, compatibility, and determinism. | `tests/unit/scheduler/*` | 5.4–5.6 | — |
| 5.8 | Implement offline llama.cpp/GGUF sharder adapter. | `python/socrates_engine/sharder.py`, tests | 1.4, 5.2c | H |
| 5.9 | Implement offline ExecuTorch/PTE sharder adapter. | Python sharder modules/tests | 1.4, 5.2c | H |
| 5.10a | Implement offline layer sensitivity profiler and fixtures. | `python/socrates_engine/sensitivity.py`, tests | 1.4 | H |
| 5.10b | Implement offline phase-specific calibration runner and fixtures. | `python/socrates_engine/calibration.py`, tests | 1.4, 5.8 or 5.9 | — |
| 5.10c | Implement deterministic manifest emitter from shard/calibration metadata. | `python/socrates_engine/manifest.py`, tests | 5.2b, 5.2c, 5.10a, 5.10b | — |

## Phase 6: Inference Backends and Pipeline

| ID | Task | Files | Dependencies | Parallel group |
|---|---|---|---|---|
| 6.1 | Implement backend registry and explicit fallback-chain validation. | `src/inference/backend_registry.cpp`, tests | 1.2, 5.2c | I |
| 6.2 | Implement llama.cpp shard session adapter and local KV lifecycle. | `src/inference/llama_backend.cpp`, tests | 5.3, 6.1 | J |
| 6.3 | Implement ExecuTorch/QNN shard session adapter. | `src/inference/executorch_qnn_backend.cpp`, tests | 5.3, 6.1 | J |
| 6.4 | Implement ExecuTorch CPU fallback adapter and failure tests. | inference source/tests | 5.3, 6.1 | J |
| 6.5 | Implement single-stage prefill/decode pipeline orchestration. | `src/pipeline/inference_pipeline.cpp`, tests | 6.2–6.4 | — |
| 6.6 | Implement remote multi-stage activation handoff. | pipeline source/tests | 4.3, 5.3, 6.5 | — |
| 6.7a | Implement active-request node-loss detection and stale-stage cancellation. | pipeline source/tests | 2.6, 6.6 | — |
| 6.7b | Implement fenced replacement-plan admission and shard reload. | pipeline source/tests | 5.1, 5.6, 6.7a | — |
| 6.7c | Implement retained-token replay and resume/terminal degradation behavior. | pipeline source/tests | 6.7b | — |
| 6.8 | Implement trace lifecycle, recorder, and export format. | `src/trace/trace_recorder.cpp`, `proto/socrates/v1/trace.proto`, tests | 1.3 | I |
| 6.9 | Instrument pipeline schedule/layer/transfer/token events. | pipeline/transport/backend sources/tests | 6.6, 6.8 | — |
| 6.10a | Implement runtime startup/rollback and snapshot publication. | `src/runtime/edge_runtime.cpp`, tests | 2.6, 3.7, 5.3, 5.6, 6.7c, 6.9 | — |
| 6.10b | Implement stop/drain, callback quiescence, and restart tests. | runtime source/tests | 6.10a | — |
| 6.10c | Implement daemon and stable C ABI shells over `EdgeRuntime`. | `daemon/*`, `bindings/c/*`, tests | 6.10b | — |

## Phase 8: Runtime Integration, Packaging, and Validation

| ID | Task | Files | Dependencies | Parallel group |
|---|---|---|---|---|
| 8.1 | Package Android AAR and verify sample consumer. | `platform/android/*`, package tests | 6.10c | N |
| 8.2 | Package iOS/macOS XCFramework and verify sample consumer. | `platform/apple/*` | 6.10c | N |
| 8.3 | Package Windows DLL/service/MSI and local management endpoint shell. | `platform/windows/*` | 6.10c | N |
| 8.4 | Create multi-node Docker image/Compose topology and deterministic fixtures. | `tests/e2e/*` | 6.10c | N |
| 8.5 | E2E: discovery, election, profile, schedule, and one streamed request. | E2E scenarios | 8.4 | — |
| 8.6 | E2E: worker loss and successful reschedule/replay. | E2E scenarios | 6.7c, 8.4 | O |
| 8.7 | E2E: leader loss, new fence, stale leader rejection. | E2E scenarios | 2.6, 8.4 | O |
| 8.9 | Build profiler/benchmark/manifest/trace CLI tools and baselines. | `tools/*`, `scripts/*` | 3.7, 5.2c, 6.8 | N |
