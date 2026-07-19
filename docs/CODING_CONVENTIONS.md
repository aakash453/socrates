# Coding Conventions and Agent Workflow

**Design baseline:** Authoritative conventions extracted from blueprint v1.0.  
**Applies to:** `socrates`, `socrates-app`, tooling, tests, generated-source inputs, and documentation.

## 1. Mandatory Agent Workflow

Every implementation agent or subagent MUST follow this workflow:

1. Read the target repository's `AGENTS.md`, blueprint, implementation task plan, and this conventions document before changing files.
2. Work on exactly one listed implementation task per session. Do not bundle unrelated tasks, formatting, or refactors.
3. Implement only the files and matching tests within that task's scope. If a prerequisite or contract is missing, report it rather than silently redefining it.
4. Preserve public API, host ABI, Protobuf, manifest, native bridge, event, persistence, and cross-repository contracts. Contract or schema changes require architecture review before implementation.
5. Add or update deterministic tests for all behavior changed. Use focused tests first, then the broader validation required by the task.
6. Run relevant formatting, lint, type, build, and test commands. Do not claim a check passed unless it was run and passed.
7. At session end, report the task ID, files changed, behavior implemented, validation commands and results, and any remaining risks or blocked dependencies.

## 2. Universal Rules

- UTF-8, LF endings, final newline, no trailing whitespace.
- Maximum line length: 100 characters in C++/Python; 100 in TypeScript except generated Codegen types.
- Public identifiers and wire fields are stable API. Renaming or changing semantics requires an ADR and compatibility review.
- Layer ranges are always `[start, end_exclusive)`; names MUST include `end_exclusive` where ambiguity exists.
- Timestamps use UTC RFC 3339 for interchange and monotonic clocks for local durations/deadlines.
- Byte sizes include `_bytes`; durations include units such as `_ms` or `_microseconds`, or use typed chrono values.
- `uint64` values crossing JavaScript are decimal strings.
- No cloud dependency, hidden telemetry, model download, prompt logging, or secret logging.

## 3. Naming

| Language/item | Rule | Example |
|---|---|---|
| C++ files | `snake_case.h/.cpp` | `leader_election.h` |
| C++ classes/structs/enums | `PascalCase` | `CapabilityProfile` |
| C++ methods/variables | `snake_case` | `create_plan` |
| C++ enum values | `kPascalCase` | `kSensitivityAware` |
| Python modules/functions/variables | `snake_case` | `create_shard` |
| Python classes | `PascalCase` | `ManifestValidator` |
| TypeScript files | `PascalCase.tsx` for UI, `PascalCase.ts` for classes, `camelCase.ts` for utilities/contracts | `ChatScreen.tsx`, `identifiers.ts` |
| TypeScript types/components | `PascalCase` | `ClusterSnapshot` |
| TypeScript functions/variables | `camelCase` | `useChatSession` |
| Protobuf messages | `PascalCase` | `ExecutionProfile` |
| Protobuf fields | `snake_case` | `end_layer_exclusive` |
| Tests | `<subject>_test.cpp`, `test_<subject>.py`, `<Subject>.test.ts` | `scheduler_test.cpp` |

## 4. Formatting and Language Rules

### 4.1 C++20

- Use Google-style braces as enforced by repository `.clang-format`; two-space indentation.
- Prefer RAII and value types. No owning raw pointers; use `std::unique_ptr` by default and `std::shared_ptr` only for shared lifetime.
- Mark single-argument constructors `explicit`, observers `[[nodiscard]]`, and non-throwing destructors/stop methods `noexcept`.
- Use `std::chrono`, `std::filesystem`, `std::span` where ownership permits, and `std::stop_token` for cooperative cancellation.
- Never hold a lock while invoking user callbacks, blocking on transport, or executing a backend.
- Public headers MUST NOT include vendor backend/platform headers.
- Exceptions MUST NOT cross C, JNI, ObjC, WinRT, gRPC, or daemon boundaries.

### 4.2 Python 3.12

- Four-space indentation; Black and Ruff formatting; strict Pyright/mypy.
- Public data objects are `@dataclass(frozen=True)` unless mutation is essential.
- Every public callable has full annotations and Google-style docstrings.
- Use `pathlib.Path`; never concatenate filesystem paths as strings.
- Catch only exceptions that can be handled or enriched; preserve causes with `raise ... from error`.

### 4.3 TypeScript and React Native

- TypeScript strict mode; two-space indentation; single quotes; trailing commas.
- No `any`; use `unknown` plus a validated codec at native/network boundaries.
- Domain objects and component props are `readonly`; stores publish immutable snapshots.
- Components are named functions. Hooks begin with `use`. Business logic belongs in services/stores, not components.
- High-frequency native data is batched; never call `setState` once per token or telemetry sample.
- Every native event subscription has deterministic cleanup for unmount, Fast Refresh, and bridge recreation.

## 5. Required API Documentation Template

Every public method MUST document:

```text
Purpose: one-sentence behavior.
Preconditions: caller obligations; write "none" when absent.
Postconditions: success and failure state guarantees.
Throws/Raises: exact exception/error mechanism and stable error categories.
Thread safety: concurrent-call and callback guarantees.
Side effects: I/O, resources, persistence, compute, permissions, or "none".
```

Comments explain intent, invariants, or trade-offs; they MUST NOT merely restate code. TODOs include an issue ID: `TODO(EDGE-123): ...`.

## 6. Error Handling

- C++ expected operational errors use `Result<T>` and stable `ErrorCode`; programmer/precondition defects MAY throw `RuntimeError` only where documented.
- Python CLI boundaries translate typed exceptions into stable non-zero exit codes; library code does not call `sys.exit`.
- TypeScript adapters convert native rejection payloads into a typed application error with `code`, `message`, `recoverable`, and `subsystem`.
- Include actionable context but not prompts, tensor bytes, secrets, or full local paths.
- Retry only explicitly retryable and idempotent operations; use bounded attempts, deadline, jitter, and cancellation.
- Never silently fall back to CPU, drop stream frames, accept checksum mismatch, or accept stale fences.

## 7. Logging and Tracing

- Use structured fields: `timestamp`, `severity`, `subsystem`, `event`, `node_id`, `request_id`, `trace_id`, `term`, `plan_id`.
- Severity: `TRACE` local detail, `DEBUG` diagnostics, `INFO` lifecycle, `WARN` recoverable degradation, `ERROR` request/component failure, `FATAL` unrecoverable process state.
- One failure is logged once at the ownership boundary; lower layers return enriched errors rather than duplicate logs.
- Use OpenTelemetry-compatible trace/span IDs. Span names follow `socrates.<subsystem>.<operation>`.
- Record durations using a monotonic clock; wall time is informational.
- Sampling and trace recording are runtime-configurable. Sensitive payload recording is prohibited by default and has no MVP switch.

## 8. Testing

- Every implementation source has a corresponding unit test file.
- C++ uses GoogleTest/GoogleMock; Python uses pytest; TypeScript uses Jest/React Native Testing Library; mobile wrappers use XCTest and Android instrumentation tests.
- Test names use `Given_When_Then` semantics, for example `ExpiredProfile_WhenScheduling_IsRejected`.
- Prefer fakes for clocks, transports, filesystems, and backends; no sleeps in unit tests.
- Scheduler tests MUST include property checks for complete coverage, no overlap, memory fit, compatibility, deterministic output, and stale-fence rejection.
- Transport tests inject loss/reorder/delay and verify explicit failure or recovery.
- Manifest validation keeps one malformed fixture per mandatory rule.
- Race-prone C++ is run under ThreadSanitizer; native memory paths under ASan/UBSan where platform support permits.
- Tests MUST be deterministic and must not require internet access.

## 9. Version Control

- Branch names: `feature/EDGE-123-short-name`, `fix/EDGE-123-short-name`, `docs/EDGE-123-short-name`.
- Conventional commits: `feat(scheduler): add memory placement`, `fix(transport): reject sequence gap`.
- One task per branch; do not mix formatting or unrelated refactors.
- Rebase before review; do not force-push shared integration branches.
- Generated files, lockfiles, schema descriptors, and manifests are committed only when their sources change.
- Protobuf field numbers are never reused. Removed fields are reserved. Run compatibility checks before merge.
- Pull requests list requirement IDs, tests run, platform impact, security/privacy impact, and migration notes.
