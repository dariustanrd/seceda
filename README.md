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
* future custom C++ targets under `src/`

Initialize submodules before configuring:

```bash
git submodule update --init --recursive
```

### Native builds

```bash
cmake --preset native-release
cmake --build --preset native-release
```

Use `native-debug` for local debugging.

### Generic ARM64 cross-compilation

The ARM presets are generic Linux ARM64 presets, not Telechips/Yocto-specific ones.

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

`src/CMakeLists.txt` is set up as a reusable template. Add custom targets under `src/` and either:

* call `add_llama_runner(...)` for `llama.cpp`-based apps, or
* call `add_executorch_runner(...)` for ExecuTorch-based apps.
