# Seceda Project Overview

This document consolidates the repository's Markdown docs into one high-level picture of Seceda: what the project is trying to become, what is currently documented, what appears to exist in this checkout, and which objective decisions still need alignment.

## Executive Summary

Seceda is intended to be a local-first, OpenAI-compatible edge inference gateway. Its core product promise is that client applications can talk to a localhost OpenAI-compatible API, while Seceda decides whether each request should run locally on an edge model or escalate to a stronger cloud model.

The long-term architecture is a hybrid edge/cloud framework:

- edge devices handle common, latency-sensitive, privacy-sensitive requests locally;
- a router decides when local inference is likely insufficient;
- cloud fallback runs through Modal-hosted inference backends such as vLLM or SGLang;
- Seceda preserves routing, fallback, timing, backend identity, and observability outside the public OpenAI response body.

The near-term product direction has shifted from a Seceda-specific inference API toward OpenAI compatibility as the main external contract. The documented current chat surface is `GET /v1/models` and `POST /v1/chat/completions`. The next major compatibility target is `POST /v1/responses` so Codex can point at Seceda as a local OpenAI-compatible endpoint.

## Current Checkout Reality

The current repository has been reconciled around a Rust-first scaffold. The visible first-class areas are:

- `crates/seceda-core/`: shared Rust request, model, routing, runtime adapter, config, and trace contracts.
- `crates/seceda-server/`: Rust server boundary scaffold for the localhost OpenAI-compatible API.
- `crates/seceda-llama/`: Rust `llama.cpp` runtime integration scaffold.
- `crates/seceda-cli/`: Rust CLI entrypoint.
- `seceda_cloud/`: Python Modal/vLLM serving scaffold.
- `seceda_tui/`: OpenTUI operator console.
- `docs/`: plans, architecture notes, package layout, use cases, and work log.
- top-level `README.md`, `pyproject.toml`, `uv.lock`, and `skills-lock.json`.

The root `pyproject.toml` now keeps only the existing Python cloud package in the `uv` workspace. Rust workspace membership is managed by the root `Cargo.toml`.

`seceda-core` now includes the first portable tracer path. It validates typed
configuration, routes a normalized request to a configured runtime, executes it
through a runtime adapter trait, and returns normalized output plus internal
trace events. The first adapter is a deterministic mock runtime for proving the
contract before server, llama.cpp, Modal, or mobile SDK dependencies are added.

The baseline router is heuristic and debuggable. It routes remote-only
capabilities, long prompts, prompts above the estimated token threshold,
structured-output keywords, freshness keywords, and cloud-complexity keywords to
cloud with explicit matched rules. Simple requests default to local. Its default
limits and keyword sets intentionally mirror the previous native heuristic
router baseline.

The first server MVP exposes a blocking headless daemon with a minimal
`POST /v1/responses` tracer. It accepts string `input`, calls `seceda-core`
through the mock runtime adapters, and returns a clean OpenAI-compatible
Responses-style body without routing metadata in the public payload.
The same tracer supports streaming with stable Responses-style SSE events while
recording request normalization, routing, runtime selection, stream delta, and
completion events internally.

The llama.cpp runtime package currently covers setup diagnostics: discovering a
`llama-server` binary on `PATH`, checking an HTTP sidecar endpoint, and
reporting runtime capabilities in the core adapter shape.


## Product Objective

Seceda should let OpenAI-compatible clients run against a local edge service while Seceda owns execution policy internally.

The core objective is:

1. Accept OpenAI-compatible requests on localhost.
2. Normalize those requests into a Seceda-owned internal inference contract.
3. Route requests across local, sidecar, SDK-bridge, or cloud backends.
4. Preserve fallback and observability at the execution layer.
5. Return clean OpenAI-compatible responses to clients without leaking Seceda-specific metadata into the public wire shape.

The highest-priority external acceptance target now appears to be real Codex compatibility through `POST /v1/responses`, including SSE streaming and function/tool-call flows.

## Architecture Intent

The intended runtime shape is:

```text
OpenAI-compatible client
  -> Seceda localhost API
  -> OpenAI transport adapter
  -> Seceda request normalizer
  -> router policy
  -> local engine registry or remote backend
  -> Seceda response/stream events
  -> OpenAI wire serializer
  -> client
```

The edge service is meant to be the primary runtime boundary. Local execution can include:

- in-process native runtimes such as `llama.cpp`;
- local sidecar servers exposing OpenAI-compatible APIs;
- SDK bridge adapters for future Cactus- or RunAnywhere-style integrations.

Cloud execution is currently Modal-centered. The cloud package is a standalone Python project for Modal and vLLM serving, with commands documented for `uv run modal run` and `uv run modal deploy`.

The TUI is an operator console for observability and configuration. It talks to the daemon endpoints such as `/info`, `/v1/models`, `/metrics`, `/metrics/events`, and config/admin endpoints.

## API Direction

### Documented Chat Surface

The top-level README positions Seceda as an OpenAI-compatible localhost edge service exposing:

- `GET /v1/models`
- `POST /v1/chat/completions`
- `GET /metrics/events?request_id=<openai_id>` for Seceda-specific request diagnostics

The expected model aliases are:

- `seceda/default` for automatic routing;
- `local/default` to force local execution;
- `remote/default` or configured remote aliases to force cloud execution.

OpenAI responses should remain clean. Seceda-specific routing decisions, matched rules, backend identity, active model path, timing, token metadata, and fallback information belong in metrics/events.

### Responses API Target

The Responses API docs make `POST /v1/responses` the next major compatibility target. The feature should be a transport adapter over the existing chat-shaped runtime, not a rewrite of Seceda around native OpenAI Responses objects.

Day-one Codex support should include:

- text input and output;
- `instructions`;
- conversation history represented as Responses `input` items;
- assistant output items;
- function calls and function-call outputs;
- streamed text deltas;
- streamed function-call argument deltas;
- reasoning-related fields Codex sends or expects where they can be preserved safely;
- OpenAI-style errors for unsupported semantic features.

The compatibility gate should be an actual local Codex smoke test pointed at Seceda. Unit tests and integration tests are necessary, but not sufficient.

## Routing Strategy

The routing docs describe Seceda as a framework rather than a fixed policy. Routing should stay pluggable and measurable.

Recommended baseline:

1. Use cheap pre-inference heuristics for obvious cloud-needed prompts.
2. Run the local SLM first when the prompt looks local-suitable.
3. Use post-generation confidence signals such as logprob, entropy, answer quality, or verifier checks.
4. Escalate to cloud when confidence is too low.

Possible routing approaches include:

- heuristic rules based on prompt length, domain, code/math/research keywords, or freshness needs;
- learned difficulty classifiers;
- embedding similarity against labeled easy/hard examples;
- tiny prompted self-classifiers;
- cost/latency/bandwidth-aware policies;
- speculative local/cloud parallelism for high-priority low-tail-latency cases;
- post-generation confidence estimators, verifiers, self-consistency, and task-specific checks.

The practical first version should favor debuggable heuristics and simple confidence checks before adding more complex learned policies.

## Use Cases

The main documented use cases are:

- smart home and IoT assistants;
- in-vehicle assistants and control copilots;
- on-device assistants for phones and laptops;
- education and tutoring;
- local coding agents on developer machines;
- distributed edge AI across many devices;
- latency-sensitive command and voice interfaces;
- routing policy research and benchmarking.

Across these use cases, the shared pattern is the same: common requests should stay local for latency, privacy, offline behavior, and cost; hard requests should escalate to cloud for quality.

The most concrete near-term product wedge in the later docs is local coding agents, because Codex compatibility is called out as the primary acceptance target for the Responses API work.

## Cloud Runtime

`seceda_cloud/` is the standalone Python project for Modal and vLLM serving. The docs describe it as a scaffold for Modal deployment:

```bash
uv sync
uv run modal run vllm_inference.py
uv run modal deploy vllm_inference.py
```

From the repo root:

```bash
uv sync --package seceda-cloud
uv run --package seceda-cloud modal run seceda_cloud/vllm_inference.py
```

The work log says the cloud implementation started from Modal's vLLM example, moved from T4 to L40S due to CUDA/version issues, and updated vLLM to support newer models such as Qwen3.5. Cold start time was observed around one minute, so Modal cold-start optimization remains an active concern.

## TUI Runtime

`seceda_tui/` is an OpenTUI operator console with two documented screens:

- `F1`: observability
- `F2`: configuration

The TUI can be configured with:

```bash
SECEDA_TUI_BASE_URL=http://127.0.0.1:8080
SECEDA_TUI_CONFIG_PATH=seceda_edge/config/seceda.toml
SECEDA_TUI_CATALOG_PATH=seceda_edge/config/config_catalog.toml
```

Important hardening notes:

- keep the base URL on loopback unless daemon access is protected by authentication and TLS;
- trace views can expose prompts, outputs, and tool arguments;
- production secrets should prefer environment variables such as `SECEDA_CLOUD_API_KEY`;
- config saving rewrites TOML and does not preserve comments or original formatting.

## Load Testing Plan

The load-testing docs frame Seceda performance around two questions:

- Can Seceda serve large fleets of edge devices?
- Does routing improve the latency/cost/quality curve?

Recommended phases:

1. Simulate 1,000 to 10,000 virtual devices with Modal plus Locust or custom Python agents.
2. Add 100 to 500 containerized devices that run real `llama.cpp` with CPU/RAM constraints.
3. Add multi-region and fault-injection tests with tools such as Toxiproxy or `tc/netem`.
4. Use real device farms only if mobile-native behavior becomes a product requirement.

Core metrics:

- local route ratio and cloud fallback ratio;
- false-local and false-cloud rates;
- p50/p95/p99 latency and time to first token;
- throughput and queue time;
- timeout, error, and retry rates;
- cloud calls and cost per 1,000 requests.

## Build And Repo Organization Intent

The current repo plan is a mixed Rust, Python, and TypeScript monorepo:

- Rust edge runtime scaffold under `crates/`;
- Python cloud tooling under `seceda_cloud/`;
- TypeScript/OpenTUI operator console under `seceda_tui/`.

The root `Cargo.toml` is the Rust workspace entrypoint. The root `pyproject.toml` remains the `uv` workspace entrypoint for Python packages that actually exist in the checkout.

## Main Roadmap

The docs imply this priority order:

1. Restore or reconcile the repo structure so implementation and docs agree.
2. Keep `POST /v1/chat/completions` and `GET /v1/models` OpenAI-compatible and spec-tight.
3. Preserve Seceda metrics/events as the separate observability surface.
4. Add stateless `POST /v1/responses` for Codex.
5. Implement Responses SSE event sequencing and function-call streaming.
6. Add explicit OpenAI-style errors for unsupported stateful, multimodal, or tool features.
7. Run a real local Codex smoke test as the compatibility gate.
8. Rebuild or evolve the local engine registry and backend identity model.
9. Expand routing quality, load testing, and cloud cold-start optimization.
10. Treat richer TUI/control-plane work as follow-on after the backend/API contracts stabilize.

## Key Risks

- The current checkout does not match the larger documented architecture.
- Full OpenAI Responses parity is much larger than Codex needs; the first pass must stay scoped.
- SSE streaming and function-call deltas require careful event ordering and stable IDs.
- Fallback after user-visible stream output can produce incoherent mixed-backend streams, so fallback should only happen before first visible text/tool delta.
- Silently ignoring advanced OpenAI fields would create false compatibility; unsupported semantic features need explicit OpenAI-style errors.
- Modal cold starts may weaken the cloud-fallback UX unless optimized.
- Routing quality needs measured false-local and false-cloud rates, not just latency and cost metrics.
- The TUI currently references `seceda_edge` config paths that are absent in this checkout.
