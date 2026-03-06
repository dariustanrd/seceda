# Edge Device Load Testing (Seceda)

This document condenses the earlier brainstorming into a practical plan for the
`seceda` project.

## Context from `README.md`

Seceda is an edge-first hybrid inference system:

- Edge device runs a local SLM (via `llama.cpp`) for fast/private responses.
- Router decides when to escalate difficult queries to a cloud LLM.
- Cloud execution is currently centered on Modal (ephemeral workers).

Load testing should therefore validate both:

1. **Serving performance** under large edge-fleet traffic.
2. **Routing quality/cost** of local-vs-cloud decisions.

## What We Need to Measure

- Can we simulate **100 to 10,000+ edge devices** realistically?
- How does latency change when fallback rate rises?
- How stable is cloud fallback under bursts?
- What is cloud cost per query as traffic mix changes?
- How accurate is routing (good local answers vs unnecessary escalations)?

## Main Approaches for Edge Load Testing

| Approach | Realism | Scale | Best Use |
| --- | --- | --- | --- |
| Virtual devices in one async runtime | Low-Medium | Very High | Fast iteration and stress tests |
| Containerized synthetic fleet (1 container ~= 1 device) | Medium | High | Better device isolation and routing behavior testing |
| CPU-constrained `llama.cpp` containers | High | Medium | Closest software-level proxy for laptops/SBC/edge boxes |
| Real device farms (phones/tablets) | Very High (mobile) | Medium | Final validation for mobile-specific behavior |

### 1) Virtual Device Simulation (async I/O)

Run thousands of "virtual devices" as state machines in one/few processes.

- **Good for:** early stress tests, routing policy tuning, burst behavior.
- **Pros:** cheapest, simplest, highest scale.
- **Cons:** does not model per-device CPU contention well.
- **Typical tools:** `Locust`, `k6`, custom async Python/Go clients.

### 2) Containerized Synthetic Edge Fleet

Each container runs an edge agent with router logic (optionally local SLM).

- **Good for:** medium/high realism with straightforward orchestration.
- **Pros:** better isolation, profile-based fleet composition.
- **Cons:** higher infra cost than pure virtual users.
- **Typical tools/platforms:** Kubernetes, Modal containers, Fly.io machines.

### 3) CPU-Constrained `llama.cpp` Device Containers

Run real local inference in each device container with capped CPU/RAM.

- **Good for:** reproducing local-model bottlenecks and route-to-cloud behavior.
- **Pros:** closest to real edge execution without physical hardware.
- **Cons:** scales less than pure HTTP/virtual-device methods.
- **Typical setup:** Docker/Kubernetes resource limits + `llama.cpp` quantized GGUF.

### 4) Real Device Fleets (Mobile Validation)

Use phone/tablet farms only when mobile hardware/network behavior must be proven.

- **Good for:** app-level/mobile network/radio validation.
- **Pros:** highest real-world fidelity for mobile clients.
- **Cons:** expensive, less flexible for custom edge-agent software.

## Providers and Services

### A) Compute Providers for Synthetic Edge Fleets

| Provider | Fit for Seceda | Notes |
| --- | --- | --- |
| **Modal** | Excellent | Best alignment with current cloud architecture in `README`; easy ephemeral scaling for both edge simulators and fallback inference. |
| **AWS ECS/Fargate** | Strong | Managed container fleets; good for repeatable large tests in AWS. |
| **Google Cloud Run** | Strong | Serverless containers with fast scale-up; useful for burst simulations. |
| **Fly.io** | Good | Region-distributed machines; useful for geo-latency experiments. |
| **Kubernetes (EKS/GKE/AKS/self-managed)** | Strong | Most control for long-running, repeatable fleet experiments. |

### B) Load Generation Platforms

| Tool/Provider | Fit for Seceda | Notes |
| --- | --- | --- |
| **Locust** | Excellent | Very flexible for "device-like" behavior and custom routing logic. |
| **k6 (self-hosted)** | Strong | Efficient high-concurrency API testing. |
| **Grafana Cloud k6** | Strong | Managed multi-region execution and dashboards. |
| **Gatling / BlazeMeter / Flood** | Medium | Mature backend load tooling; less edge-behavior customization than Locust + custom agents. |

### C) Real Device Farm Providers (Mobile-Focused)

| Provider | Best for | Caveats |
| --- | --- | --- |
| **AWS Device Farm** | Real Android/iOS parallel runs | Primarily mobile app testing workflows |
| **BrowserStack** | Large browser/device matrix | Best for UI/app validation rather than custom edge daemons |
| **Sauce Labs** | Enterprise mobile/web test coverage | Cost and workflow overhead |
| **LambdaTest / pCloudy** | Cross-device automation | Similar limitations for custom edge-agent scenarios |

### D) Network/Fault Simulation

- **Toxiproxy**: introduce latency, jitter, packet loss between edge agents and server.
- **Linux `tc/netem`**: low-level network emulation in containers/pods.

### E) LLM-Specific Benchmark Helpers

- **GenAI-Perf (NVIDIA)**: TTFT, token throughput, concurrency.
- **LLMPerf (vLLM ecosystem)**: realistic prompt datasets and serving benchmarks.

## Recommended Plan for Seceda

### Phase 1: Fast Baseline (Now)

Use **Modal + Locust/custom Python edge agents**.

- Simulate 1,000 to 10,000 virtual devices.
- Implement device profiles (phone/laptop/iot), prompt mix, and burst waves.
- Keep local SLM optional/mocked initially to validate end-to-end routing path.

### Phase 2: Hybrid Realism

Add real `llama.cpp` execution for a subset fleet.

- 100 to 500 containerized edge devices.
- CPU/RAM-constrained profiles per device class.
- Measure local latency vs cloud fallback tradeoffs.

### Phase 3: Global/Failure Testing

- Multi-region fleet (e.g., Modal regions or Fly.io + cloud backend region).
- Inject adverse network conditions (Toxiproxy/netem).
- Validate resilience under spikes and partial outages.

### Phase 4: Optional Mobile Validation

If product direction requires mobile-native behavior, run targeted checks on
AWS Device Farm/BrowserStack/Sauce Labs.

## Workload Design for Seceda

Use three traffic classes:

1. **Local-Easy** (expected local SLM success)
   - Short FAQs, simple commands, narrow-domain requests.
2. **Cloud-Hard** (expected fallback)
   - Long context, reasoning-heavy, tool-like, or multi-step queries.
3. **Mixed Reality**
   - Production-like blend with diurnal patterns and burst events.

Also vary:

- prompt lengths and output lengths,
- think time between requests,
- streaming vs non-streaming responses,
- intermittent offline/reconnect behaviors.

## Core Metrics

### Routing and Quality

- Local route ratio, cloud fallback ratio
- False-local rate (should have escalated)
- False-cloud rate (unnecessary escalation)

### Performance

- End-to-end latency (p50/p95/p99)
- TTFT (time to first token)
- Tokens/sec throughput
- Queue/wait time at gateway and cloud serving layer

### Reliability

- Timeout/error rate by device profile and region
- Retry rate and success-after-retry

### Cost

- Cloud calls per 1,000 requests
- Estimated cloud spend per workload profile
- Cost-latency-quality tradeoff curve

## Suggested Default Stack (Project-Aligned)

For the current `README` direction, start with:

- **Edge fleet simulation:** Locust + custom Python agent
- **Fleet runtime:** Modal CPU containers (or Kubernetes if preferred)
- **Cloud fallback:** Modal GPU endpoint (vLLM/SGLang/API adapter)
- **Metrics:** Prometheus + Grafana (+ optional OpenTelemetry traces)
- **Fault injection:** Toxiproxy

This gives a practical path from quick synthetic testing to realistic hybrid
edge-cloud experiments without overcommitting to complex infrastructure early.
