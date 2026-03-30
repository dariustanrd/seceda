# seceda

**Edge-first LLM inference with intelligent cloud fallback.**

Seceda is a framework for **hybrid edge–cloud LLM inference**. It allows edge devices to run **Small Language Models** (SLMs) for fast, private responses, while **escalating difficult queries to powerful cloud-hosted models when necessary**.

Like the famous ridgeline of Seceda in the Dolomites, the system sits **between the edge and the cloud** — handling what it can locally, and elevating into the cloud when needed.

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

1. Edge-first inference with **local SLMs**
2. Local **automatic routing** to cloud LLMs
3. Support for **distributed edge devices** with **llama.cpp runtimes**
4. **Modal-powered ephemeral cloud execution**
    - vLLM / SGLang (future)
5. **Dynamic cloud model tiering** (future)
    - Select model size and GPU class based on request difficulty.
6. Easy to deploy for **experiments or demos**
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

The repository now includes a top-level CMake build that can orchestrate:

* `thirdparty/llama.cpp`
* `thirdparty/executorch`
* first-party edge C++ targets under `seceda_edge/cpp/`

Initialize submodules before configuring:

```bash
git submodule update --init --recursive
```

### ExecuTorch and Python (uv)

ExecuTorch’s C++ configure step expects a **Python 3.11+** interpreter that can `import torch` (used to locate PyTorch headers when optimized kernels are enabled). That matches the workspace packages (`requires-python = ">=3.11"`).

CMake picks an interpreter in this order:

1. `-DPython3_EXECUTABLE=...` on the configure line (e.g. from `scripts/build.sh --python`)
2. Environment variable `SECEDA_PYTHON`
3. `./.venv/bin/python3` (or `python`) when present
4. Otherwise CMake’s default search (may select an older system Python on macOS)

Recommended: use the repo virtualenv and install torch there:

```bash
uv sync --group executorch-build
```

Then configure as usual, or use `scripts/build.sh`, which applies the same preference order for preflight checks.

If `uv lock` or `uv sync` fails with **dns error** / **failed to lookup address information** when fetching `torch` from `files.pythonhosted.org`, the issue is network reachability to PyPI (offline machine, broken DNS, VPN, or corporate proxy), not the repo layout. Fix DNS/connectivity, or run `uv sync --group executorch-build` on a network you trust and commit the updated `uv.lock`. As a last resort, install PyTorch into the same interpreter CMake uses (e.g. `.venv`) with `pip install torch` and point CMake at that Python via `--python` or `SECEDA_PYTHON`.

### Native builds

```bash
cmake --preset native-release
cmake --build --preset native-release
```

Use `native-debug` for local debugging.

### Apple Silicon builds

On macOS, use the Apple-specific presets instead of the generic native presets when you want Apple backend support wired in.

Standard Apple Silicon build:

```bash
cmake --preset apple-silicon-release
cmake --build --preset apple-silicon-release
```

Available Apple presets:

* `apple-silicon-debug`
* `apple-silicon-release`
* `apple-silicon-metal-experimental`

These presets target `arm64` and currently configure:

* `llama.cpp` with Metal enabled
* ExecuTorch with Apple support enabled via CoreML and MPS
* OpenMP disabled by default to avoid requiring a separate `libomp` setup on macOS

`apple-silicon-metal-experimental` additionally enables the ExecuTorch Metal backend.
That backend is still considered experimental, so prefer `apple-silicon-release` unless you specifically want to evaluate it.

### Generic ARM64 cross-compilation

The ARM presets are generic Linux ARM64 presets.

```bash
export ARM_GNU_TOOLCHAIN_ROOT=/opt/toolchains/aarch64-linux-gnu
export ARM_SYSROOT=/opt/sysroots/aarch64-linux-gnu
# Optional if your toolchain uses a different prefix:
export ARM_TARGET_TRIPLE=aarch64-linux-gnu

cmake --preset arm64-release
cmake --build --preset arm64-release
```

Available ARM64 presets:

* `arm64-release`
* `arm64-vulkan`
* `arm64-kleidi`
* `arm64-kleidi-vulkan`
* `arm64-llama-only`
* `arm64-executorch-only`

If you use a Vulkan preset, make sure `glslc` is available on `PATH` or provide `VULKAN_SDK`.

### Custom C++ targets

`seceda_edge/cpp/CMakeLists.txt` is the first-party native entrypoint. Add edge runtime code under `seceda_edge/cpp/` and either:

* call `add_llama_runner(...)` for `llama.cpp`-based apps, or
* call `add_executorch_runner(...)` for ExecuTorch-based apps.

### Edge daemon to Modal SGLang

The current edge daemon can call a deployed Modal-backed SGLang URL directly via
the OpenAI-compatible `POST /v1/chat/completions` interface. The shared
edge-to-cloud contract lives in `seceda_shared/README.md`.

For quick local iteration on the edge daemon alone, it is often simpler to
disable ExecuTorch and build the first-party C++ targets plus `llama.cpp`:

```bash
scripts/build.sh -p apple-silicon-release --cmake-arg -DSECEDA_BUILD_EXECUTORCH=OFF
```

Start the daemon with a local GGUF model and the deployed Modal flash URL:

```bash
export MODAL_FLASH_URL="https://<your-modal-url>"
# Optional when the deployed endpoint is protected by bearer auth:
export SECEDA_CLOUD_API_KEY="<token>"

./build/apple-silicon-release/seceda_edge/cpp/apps/seceda_edge_daemon/seceda_edge_daemon \
  --model-path ./models/LFM2-1.2B-GGUF/LFM2-1.2B-Q8_0.gguf \
  --cloud-base-url "${MODAL_FLASH_URL}" \
  --cloud-model seceda-cloud-default \
  --cloud-connect-timeout-seconds 10 \
  --cloud-timeout-seconds 120 \
  --cloud-retry-attempts 2 \
  --cloud-retry-backoff-ms 1000 \
  --cloud-send-modal-session-id true
```

Single-turn smoke path:

1. Verify the deployed Modal endpoint is up:

```bash
curl "${MODAL_FLASH_URL}/health"
```

2. Verify the edge daemon is up:

```bash
curl http://127.0.0.1:8080/health
```

3. Force one cloud-routed inference through the edge daemon:

```bash
curl http://127.0.0.1:8080/inference \
  -H "Content-Type: application/json" \
  -d '{
    "text": "Explain why request-scoped Modal session ids are useful.",
    "route_override": "cloud",
    "options": {
      "max_tokens": 96
    }
  }'
```

4. Confirm the response shows `"ok": true`, `"final_target": "cloud"`, and a
   non-empty `"text"` field.

For the first milestone, the edge daemon generates a fresh `Modal-Session-ID`
per logical inference request and reuses it only across retries for that same
request. A future multi-turn upgrade will let callers supply a stable
interaction-scoped session identifier.

### Python Projects

The repo-root `pyproject.toml` is a `uv` workspace entrypoint for two standalone
Python projects:

* `seceda_cloud/` for Modal and vLLM tooling
* `seceda_edge/` for edge-side Python helpers

Install both projects together from the repository root:

```bash
uv lock
uv sync --all-packages
```

Run project-specific commands from the repository root:

```bash
uv run --package seceda-cloud modal run seceda_cloud/vllm_inference.py
uv run --package seceda-edge python seceda_edge/test_client.py
```

Each project also works on its own for deployment or isolated development:

```bash
cd seceda_edge
uv sync
uv run python test_client.py
```
