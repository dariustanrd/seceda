---
name: Migration Continuation
overview: Continue from the integrated OpenAI-compatible first pass by finishing the remaining phase-1 gaps and then extending the runtime toward real multi-engine local and named-backend support. The main unfinished areas are true incremental streaming through executors and adapters, non-stub local adapters, a dedicated config/tuning catalog for better UX, richer backend/config evolution, stronger observability labels, and docs that make the OpenAI contract primary.
todos:
  - id: finish-streaming-contract
    content: Implement true incremental streaming through executors and adapters, with dedicated streaming tests and explicit cancellation/fallback rules.
    status: completed
  - id: real-sidecar-adapter
    content: Replace one local-engine stub with a real sidecar-server adapter while keeping llama.cpp as the default in-process adapter.
    status: completed
  - id: config-ux-catalog
    content: Create a separate source-of-truth catalog for tunable parameters and configs so CLI, docs, and a later TUI can reference the same metadata.
    status: completed
  - id: named-backend-config
    content: Extend config and identity plumbing from one remote block toward a minimal named backend/engine catalog without destabilizing current routing.
    status: completed
  - id: metrics-identity-upgrade
    content: Promote engine/backend/model identity from events into more useful metrics and observability surfaces.
    status: completed
  - id: docs-openai-first
    content: Rewrite README and migration docs so OpenAI endpoints are primary and /inference is explicitly removed after the cutover.
    status: completed
isProject: false
---

# Seceda Migration Continuation

## Current State

- Completed:
  - OpenAI transport extraction under `[seceda_edge/cpp/src/openai_compat/](seceda_edge/cpp/src/openai_compat/)`
  - `POST /v1/chat/completions` and `GET /v1/models` in `[seceda_edge/cpp/apps/seceda_edge_daemon/main.cpp](seceda_edge/cpp/apps/seceda_edge_daemon/main.cpp)`
  - OpenAI-shaped internal envelope and Seceda metadata in `[seceda_edge/cpp/src/runtime/contracts.hpp](seceda_edge/cpp/src/runtime/contracts.hpp)`
  - Routing/identity plumbing through `[seceda_edge/cpp/src/runtime/request_executor.cpp](seceda_edge/cpp/src/runtime/request_executor.cpp)`, `[seceda_edge/cpp/src/router/heuristic_router.cpp](seceda_edge/cpp/src/router/heuristic_router.cpp)`, and telemetry
  - Local engine registry scaffold in `[seceda_edge/cpp/src/local_models/local_engine_registry.cpp](seceda_edge/cpp/src/local_models/local_engine_registry.cpp)`
  - HTTP compatibility tests in `[seceda_edge/cpp/tests/openai_compat_test.cpp](seceda_edge/cpp/tests/openai_compat_test.cpp)` and `[seceda_edge/cpp/tests/openai_http_integration_test.cpp](seceda_edge/cpp/tests/openai_http_integration_test.cpp)`
- Completed in this continuation:
  - True incremental streaming now runs through executors and adapters, with SSE emitted directly from execution rather than buffered response shaping
  - Post-stream-start cancellation, fallback, and error rules are now enforced in executor/runtime flow
  - `sidecar_server` is now a real localhost OpenAI-compatible local adapter behind `[seceda_edge/cpp/src/local_models/local_engine_registry.cpp](seceda_edge/cpp/src/local_models/local_engine_registry.cpp)`
  - User-facing tunables now have a dedicated metadata source of truth under `[seceda_edge/cpp/src/config_catalog/](seceda_edge/cpp/src/config_catalog/)`
  - Config can now describe named remote backends and a local engine catalog while preserving one active local runtime path
  - Metrics and `/metrics/events` now promote backend, engine, model alias, display name, and execution mode identity
  - `[README.md](README.md)` now presents the OpenAI surface as the primary integration path and documents `request_id`-based observability as the replacement for legacy bridge diagnostics

## Remaining Delivery Slices

### 1. Finish Phase 1 Streaming

- Define the streaming contract explicitly around true incremental execution, not buffered SSE synthesis, using the already-extracted transport layer in `[seceda_edge/cpp/src/openai_compat/openai_wire.cpp](seceda_edge/cpp/src/openai_compat/openai_wire.cpp)`.
- Introduce a stream-capable runtime path between transport and adapters while preserving the existing non-streaming path:
  - local executors must emit token/delta events incrementally during generation
  - cloud executors must forward upstream SSE deltas incrementally instead of buffering them into a completed response
  - transport must serialize those incremental events directly to the client as OpenAI-compatible SSE chunks
- Add explicit rules for:
  - chunk framing and `[DONE]`
  - `stream_options.include_usage`
  - client disconnect/cancellation
  - fallback before first byte vs after first byte
  - where Seceda-native metadata is allowed during streaming
- Set the default safety rule now: fallback is allowed only before the first user-visible streamed chunk; after the first streamed content/tool-call delta, the request must stay on the chosen backend and surface errors in-stream.
- Add dedicated streaming fixtures and integration tests alongside the current OpenAI HTTP tests, including:
  - local incremental streaming
  - cloud incremental streaming
  - disconnect/cancellation
  - pre-first-byte fallback behavior
  - post-first-byte error behavior

### 2. Establish A Config UX Catalog

- Introduce a separate source-of-truth config metadata layer in a dedicated location such as `seceda_edge/cpp/src/config_catalog/`, instead of continuing to grow `[seceda_edge/cpp/src/runtime/runtime_config.cpp](seceda_edge/cpp/src/runtime/runtime_config.cpp)` as the only place where tunables are described.
- Define user-facing config metadata for each tunable parameter and runtime setting, including:
  - stable key/id
  - human-readable label and description
  - type, enum values, numeric range, units, and default
  - source mapping for CLI flag, env var, config-file key, and future TUI field id
  - grouping/section such as local runtime, sidecar adapter, remote backend, routing, generation, and observability
  - whether a setting is secret/sensitive
  - whether it is hot-reloadable or requires restart/reload
  - whether it is advanced/experimental or deprecated
- Treat that catalog as the reference point for future TUI/control-plane work, even if the TUI itself remains out of scope for phase 1.
- Refactor CLI help, runtime parsing, validation, and later docs generation so they consume the same catalog metadata rather than duplicating descriptions in multiple places.

### 3. Convert Registry Scaffolding Into One Real Second Adapter

- Keep `[seceda_edge/cpp/src/local_models/llama_runtime.cpp](seceda_edge/cpp/src/local_models/llama_runtime.cpp)` as the in-process baseline.
- Implement one real non-llama adapter first as a localhost OpenAI-compatible `sidecar_server` adapter so the registry proves its shape without forcing SDK work first.
- Assume the sidecar is a separately started `llama-server`-style process on a configured localhost port, serving a different model in its own process.
- Treat that sidecar server as externally managed in this phase: Seceda forwards normalized requests to it, but does not start, stop, or own its model lifecycle.
- Use the sidecar path instead of the built-together in-process llama client for that adapter shape.
- Keep `sdk_bridge` adapters as placeholders until sidecar behavior and config stabilize.
- Ensure adapter selection remains behind `[seceda_edge/cpp/src/local_models/local_engine_registry.cpp](seceda_edge/cpp/src/local_models/local_engine_registry.cpp)`, not leaked into transport code.

### 4. Evolve Config From Single Remote Block To Named Identities

- Extend runtime config so remote backend identity is no longer just one cloud block in `[seceda_edge/cpp/src/runtime/runtime_config.cpp](seceda_edge/cpp/src/runtime/runtime_config.cpp)`.
- Add a minimal named-backend/alias layer while keeping phase-1 runtime behavior simple.
- Preserve compatibility by keeping one default remote backend and one default local engine active path, but make config capable of describing more than one backend/engine entry.
- Add the minimum local sidecar config needed for the first real second adapter: localhost base URL/port, model alias/identity, and execution mode selection.
- Express new sidecar, generation, routing, and backend options through the config catalog first, then bind runtime parsing to that catalog so future UX work reuses the same shape.
- Keep `RouteTarget` stable for now while strengthening `engine_id`, `backend_id`, and `model_alias` selection and reporting.

### 5. Strengthen Routing And Observability Around Identity

- Keep heuristics simple in `[seceda_edge/cpp/src/router/heuristic_router.cpp](seceda_edge/cpp/src/router/heuristic_router.cpp)`, but let explicit alias and preference hints reliably influence selection.
- Extend metrics beyond coarse target buckets where it adds value, using the identity fields already present in events.
- Preserve existing event log behavior while making engine/backend/model identity first-class in dashboards and debugging paths.

### 6. Finish Docs And Migration Positioning

- Rewrite `[README.md](README.md)` so Seceda is presented first as an OpenAI-compatible local edge service.
- Replace the primary `curl` examples with `GET /v1/models` and `POST /v1/chat/completions`.
- Finish the cutover by removing `/inference` once OpenAI chat responses can be correlated to `/metrics/events` by `request_id`.
- Add a short architecture note covering:
  - OpenAI transport layer
  - normalized internal envelope
  - local engine registry
  - current `llama.cpp` adapter and planned Cactus / RunAnywhere / sidecar shapes
  - localhost no-auth posture for phase 1
  - where the dedicated config/tuning catalog lives and how later UX surfaces should consume it

## Implementation Order

1. Lock the true incremental streaming contract and runtime interfaces first, because it is the biggest remaining phase-1 compatibility gap and affects both executors.
2. Introduce the dedicated config/tuning catalog early so new sidecar, backend, and routing settings are added in one reusable place instead of spreading across parser/help/docs code.
3. Expand config/backend identity shape just enough to support named entries and sidecar settings without destabilizing request execution.
4. Implement one real sidecar local adapter against an externally managed localhost `llama-server`-style process to validate the local registry design.
5. Add observability label improvements after the backend/engine identities are stable.
6. Update README and migration docs last so they reflect the final merged behavior rather than intermediate scaffolding.

## Key Integration Files

- Transport: `[seceda_edge/cpp/src/openai_compat/openai_parse.cpp](seceda_edge/cpp/src/openai_compat/openai_parse.cpp)`, `[seceda_edge/cpp/src/openai_compat/openai_wire.cpp](seceda_edge/cpp/src/openai_compat/openai_wire.cpp)`, `[seceda_edge/cpp/src/openai_compat/openai_errors.cpp](seceda_edge/cpp/src/openai_compat/openai_errors.cpp)`, `[seceda_edge/cpp/apps/seceda_edge_daemon/main.cpp](seceda_edge/cpp/apps/seceda_edge_daemon/main.cpp)`
- Runtime/execution: `[seceda_edge/cpp/src/runtime/contracts.hpp](seceda_edge/cpp/src/runtime/contracts.hpp)`, `[seceda_edge/cpp/src/runtime/request_executor.cpp](seceda_edge/cpp/src/runtime/request_executor.cpp)`, `[seceda_edge/cpp/src/cloud_bridge/cloud_client.cpp](seceda_edge/cpp/src/cloud_bridge/cloud_client.cpp)`
- Local engines: `[seceda_edge/cpp/src/local_models/local_engine_registry.cpp](seceda_edge/cpp/src/local_models/local_engine_registry.cpp)`, `[seceda_edge/cpp/src/local_models/local_engine_resolve.cpp](seceda_edge/cpp/src/local_models/local_engine_resolve.cpp)`, `[seceda_edge/cpp/src/local_models/stub_local_engine_adapter.cpp](seceda_edge/cpp/src/local_models/stub_local_engine_adapter.cpp)`
- Config/router/metrics: `[seceda_edge/cpp/src/runtime/runtime_config.cpp](seceda_edge/cpp/src/runtime/runtime_config.cpp)`, `[seceda_edge/cpp/src/router/heuristic_router.cpp](seceda_edge/cpp/src/router/heuristic_router.cpp)`, `[seceda_edge/cpp/src/telemetry/metrics_registry.cpp](seceda_edge/cpp/src/telemetry/metrics_registry.cpp)`, and a new dedicated config metadata area such as `seceda_edge/cpp/src/config_catalog/`
- Docs/tests: `[seceda_edge/cpp/tests/openai_http_integration_test.cpp](seceda_edge/cpp/tests/openai_http_integration_test.cpp)`, `[seceda_edge/cpp/tests/fixtures/openai/](seceda_edge/cpp/tests/fixtures/openai/)`, `[README.md](README.md)`

## Guardrails

- Do not redesign the stabilized public request/response contract in `contracts.hpp` unless streaming or named-backend work proves a concrete incompatibility.
- Do not implement Seceda-managed tool loops in phase 1.
- Do not broaden the OpenAI API surface beyond the agreed endpoints until streaming and docs are complete.
- Treat sidecar server lifecycle as external in this phase; adapter work should focus on request forwarding, response normalization, and selection/config plumbing.
- Keep user-facing tunables and configuration metadata centralized in the dedicated catalog rather than duplicating labels/defaults/validation rules across CLI help, docs, runtime parsing, and later TUI code.
- Keep the lead integration boundary: transport, runtime envelope, routing semantics, and final executor behavior stay centrally reviewed even if bounded subagents are used again.

