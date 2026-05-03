# seceda

**OpenAI-compatible local edge service with local-first routing.**

Seceda is being rewritten around a Rust workspace for the localhost edge service while preserving the existing Python Modal cloud runtime and OpenTUI operator console as first-class packages.

Like the famous ridgeline of Seceda in the Dolomites, the system sits **between the edge and the cloud** — handling what it can locally, and elevating into the cloud when needed.

The intended inference surface remains the OpenAI-compatible pair `GET /v1/models` and `POST /v1/chat/completions`. Use `GET /metrics/events?request_id=<openai_id>` when you need Seceda-specific routing and timing diagnostics for a completed request.

---

## Repository Layout

The current checkout is organized as:

- `crates/seceda-core`: Rust contracts shared by the server, runtimes, and CLI.
- `crates/seceda-server`: Rust server boundary for the localhost OpenAI-compatible API.
- `crates/seceda-llama`: Rust `llama.cpp` runtime integration scaffold.
- `crates/seceda-cli`: Rust `seceda` CLI entrypoint.
- `seceda_cloud/`: Python Modal and vLLM cloud runtime package.
- `seceda_tui/`: OpenTUI operator console package.
- `docs/PACKAGE_LAYOUT.md`: package commands and workspace notes.

---

## Motivation

Running SLMs locally on edge devices has clear advantages:

* **Low latency**
* **Improved privacy**
* **Reduced cloud costs**
* **Offline capability**

However, small local models often struggle with more complex queries.

Seceda solves this by introducing **adaptive routing between local and cloud models**:

1. User has a query on the edge
2. Evaluate locally whether the query is too difficult for the SLM [Option 1]
3. Run inference locally using a small model (e.g. via `llama.cpp`)
4. Evaluate locally whether the response is sufficient [Option 2]
3. If not, escalate the request to a cloud LLM
4. Return the best answer to the user

This allows systems to be **fast and cheap most of the time**, while still having **access to powerful models when needed**.

---

## Architecture

```
User Query
    │
    ▼
Edge Device (evaluator) [Option 1]
    │
    ├── confident → call SLM
    │
    └── uncertain → call cloud helper
    │
    ▼
Edge Device (SLM via llama.cpp)
    │
    ▼
Edge Device (evaluator) [Option 2]
    │
    ├── SLM was confident → return answer
    │
    └── SLM was uncertain → call cloud helper
                        │
                        ▼
                  Modal Cloud
                  (Large LLM)
                        │
                        ▼
                   Final Answer
```

Edge devices handle the majority of requests locally.
More complex tasks are sent to **ephemeral cloud workers**.

The result is a system that is:

* **Fast** for simple queries
* **Powerful** for complex ones
* **Cost-efficient**
* **Scalable across many edge devices**

---

## Cloud Execution with Modal

Seceda uses Modal to run cloud inference.

Modal allows Seceda to:

* Spin up **ephemeral GPU workers**
* Variably run **different LLMs on demand with different hardware requirements based on user needs**
* Scale automatically as more edge devices connect
* Avoid maintaining always-on GPU servers

Instead of provisioning infrastructure, the edge client simply **calls a Modal endpoint**, which runs the cloud model and returns the result.

Example flow:

```
[Local] Edge device
   │
   ▼
[Local] Seceda Router
   │
   ▼
[Local] Modal Function
   │
   ▼
[Cloud] Cloud LLM (vLLM / SGLang / API)
```

This architecture makes it easy to **deploy hybrid edge-cloud inference without managing servers**.

---

## Features

1. OpenAI-compatible localhost edge service for `chat.completions`
2. Edge-first inference with **local SLMs**
3. Local **automatic routing** to cloud LLMs
4. Support for **distributed edge devices** with **llama.cpp** and localhost sidecar runtimes
5. **Modal-powered ephemeral cloud execution**
    - vLLM / SGLang (future)
6. **Dynamic cloud model tiering** (future)
    - Select model size and GPU class based on request difficulty.
7. Easy to deploy for **experiments or demos**
    - Fleet-scale operation synthetic load testing.
    - Demo-friendly user interface

## Framework Philosophy

Seceda is intended as a framework, not a fixed policy:

- open interfaces for edge and cloud adapters,
- pluggable router contracts for custom or proprietary logic,
- reference policies for experimentation and benchmarking.

---

## Example Use Cases

* AI assistants running on **phones, laptops, or even in your cars**
* **On-device copilots** with cloud fallback
* **Edge AI deployments** across many devices
* Hybrid inference for **latency-sensitive systems**
* Research on **LLM routing and hybrid inference**


---

## Roadmap

* Edge client with `llama.cpp`
* Routing strategies for query difficulty
* Modal cloud inference backend
* Load testing with many simulated edge devices
* Visualization dashboard for routing decisions

---

## Build System

The repository now includes a top-level Cargo workspace for the Rust rewrite:

```bash
cargo test --workspace
cargo run -p seceda-cli -- --help
cargo run -p seceda-cli -- models
```

The preserved Python cloud runtime is still managed through `uv`:

```bash
uv sync --package seceda-cloud
uv run --package seceda-cloud modal run seceda_cloud/vllm_inference.py
```

The preserved TUI is still managed through Bun from `seceda_tui/`:

```bash
bun install
bun test
bun run check
```
