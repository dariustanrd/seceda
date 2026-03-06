# Routing Approaches Brainstorming (Edge SLM + Cloud LLM)

Goal: decide when a query should stay on-device (SLM via `llama.cpp`) versus escalate to a cloud LLM.

---

## Before SLM inference (pre-generation routing)

Routing decision happens before the edge SLM generates an answer.

### 1) Heuristic / rule-based gate

Cheap deterministic checks on the prompt:

- Prompt length over threshold (tokens or chars)
- Presence of reasoning/code/math/research keywords
- Domain tags (legal, medical, finance, compliance)
- Structured output requirement (SQL, code, JSON schema, formal proof)
- External knowledge freshness requirement (time-sensitive queries)

Example:

```python
if token_length > 512:
    route_to_cloud()
elif contains_code_or_math(query):
    route_to_cloud()
else:
    route_to_slm()
```

Why use it:

- Very fast
- Easy to deploy and debug
- Good first line of defense

Risks:

- Brittle generalization
- Requires manual tuning and maintenance

---

### 2) Difficulty classifier router (learning-to-defer)

Train a small model to predict whether SLM is likely to succeed:

`difficulty(query) -> {local_ok, cloud_needed}`

Potential features:

- Query length and structure features
- Embedding vectors
- Prompt type/domain metadata
- Historical SLM success/failure labels

Model choices:

- Logistic regression on embeddings
- Tiny transformer / TinyBERT
- Lightweight MLP

Why use it:

- Better than static rules once trained
- Low inference overhead

Risks:

- Needs labeled data
- Can drift if traffic changes

---

### 3) Embedding similarity routing

Compare query embedding with previously labeled "easy for SLM" and "hard for SLM" queries.

Flow:

`query -> embedding -> nearest neighbors -> route decision`

Infra options:

- `llama.cpp` embeddings
- Sentence-transformers embeddings
- FAISS / vector DB index

Why use it:

- Works with modest labeled data
- Intuitive and explainable via nearest examples

Risks:

- Sensitive to embedding quality and dataset coverage
- Needs periodic index refresh

---

### 4) Prompted self-difficulty check (EASY/HARD classifier prompt)

Run a tiny "can I answer this?" prompt before answering:

```text
System: Return ONLY EASY or HARD for this question.
User: <query>
```

If `HARD`, route to cloud.

Why use it:

- Very simple to add
- Often useful as a quick gate

Risks:

- Depends on SLM meta-reasoning ability
- Can be over- or under-confident

---

### 5) Cost/latency/bandwidth-aware policy router

Router objective explicitly optimizes:

`quality - (lambda_cost * cloud_calls) - (lambda_latency * delay)`

Signals:

- Device load (CPU/GPU, memory, battery)
- Network quality and bandwidth cost
- User tier/SLA latency targets

Why use it:

- Closer to real production constraints
- Allows dynamic routing policy per environment

Risks:

- More complex policy tuning
- Needs robust telemetry

---

### 6) Speculative/parallel pre-routing variant

Run SLM and cloud LLM in parallel, then choose/merge result.

Why use it:

- Lowest tail latency for critical requests

Risks:

- Highest compute cost
- More orchestration complexity

---

## After SLM inference (post-generation routing)

SLM answers first, then system decides whether to accept or escalate.

### 1) Confidence estimator on generated answer

Estimate:

`P(answer is correct | query, slm_output)`

Common signals:

- Average token logprob
- Token entropy
- Per-token max probability statistics
- Answer length and structure quality

Example:

```python
confidence = w1 * avg_logprob + w2 * inverse_entropy + w3 * length_score
if confidence < tau:
    route_to_cloud()
```

Why use it:

- Uses answer-level evidence, not just prompt clues
- Core mechanism in many cascade systems

Risks:

- Raw confidence can be miscalibrated
- Needs threshold calibration per model/task

---

### 2) Two-stage answer + verification

Pipeline:

1. SLM generates answer
2. Verifier checks answer (`PASS/FAIL`)
3. Escalate on fail

Verifier options:

- Same SLM as judge
- Separate tiny verifier model
- Rule-based checker

Why use it:

- Catches obvious hallucinations
- Improves reliability versus single pass

Risks:

- Extra latency/cost on device
- Judge quality can bottleneck system quality

---

### 3) Self-consistency voting

Generate multiple sampled answers and check agreement.

High agreement -> accept  
Low agreement -> escalate

Why use it:

- Strong uncertainty signal for reasoning tasks

Risks:

- Multiple SLM runs increase latency
- Still not a correctness proof

---

### 4) Perplexity/uncertainty thresholding

Use output perplexity (or related uncertainty) as escalation trigger.

Why use it:

- Simple quantitative trigger

Risks:

- Task-dependent calibration required
- Can fail on short but wrong answers

---

### 5) Task-specific post-checks

Validate answer against task constraints before returning.

Examples:

- Code: run compile/test checks
- Math: symbolic or numeric consistency checks
- Structured output: schema validation
- Retrieval tasks: citation/grounding checks

Why use it:

- High precision for constrained tasks

Risks:

- Requires task-specific tooling
- Not always applicable to open-ended QA

---

### 6) Feature-based confidence classifier

Train post-generation classifier on features from prompt + answer:

- `avg_logprob`
- `entropy`
- `prompt_length`
- `answer_length`
- formatting/consistency flags

Output:

`accept` vs `escalate`

Why use it:

- Better calibrated than single raw metric

Risks:

- Needs labeled outcomes
- Retraining needed as models/prompts evolve

---

### 7) Step-level routing during reasoning

Instead of routing whole queries, route at intermediate reasoning steps.

Pattern:

- SLM handles easy steps
- Cloud LLM handles uncertain/critical steps

Why use it:

- Better cost-quality trade-off for long reasoning tasks

Risks:

- Most complex orchestration path
- Harder implementation and observability

---

## Practical baseline for `llama.cpp` edge deployment

Recommended layered rollout:

1. Pre-inference heuristics (cheap obvious rejects)
2. SLM first-pass answer
3. Post-inference confidence check (logprob + entropy)
4. Optional verifier for high-risk domains
5. Cloud fallback when confidence below threshold

This usually provides the best initial trade-off across latency, cost, and quality.

---

## Research/problem naming to use in notes and search

- LLM routing / model routing
- LLM cascades / model cascades
- Adaptive model selection
- Selective prediction / reject option
- Learning to defer
- Cost-aware inference
- Edge-cloud model routing

---

## Brainstorming prompts for next iteration

- Which query classes should always bypass SLM?
- What confidence metric calibrates best for our chosen SLM?
- How should thresholds adapt to device load and network quality?
- Should we optimize for fixed quality budget or fixed latency SLO?
- Which tasks justify step-level routing complexity?

---

## References and advanced notes

### Confidence estimator vs pre-inference routers

Key difference is **decision timing** and whether the router sees generated output.

| Approach | Decision time | Uses SLM output |
| --- | --- | --- |
| Heuristic rules | Before inference | No |
| Difficulty classifier | Before inference | No |
| Prompted self-difficulty (EASY/HARD) | Before inference | No |
| Confidence estimator (cascade) | After inference | Yes |

Interpretation:

- Pre-inference routing is cheaper and faster.
- Post-inference routing has more evidence and can be more accurate.
- In practice, hybrid pipelines often combine both.

### Seminal and SOTA-style directions

Core foundations:

- Selective prediction / reject option
- Learning to defer
- Cost-sensitive inference

Frequently cited LLM routing/cascade work:

- Unified Routing and Cascading (ETH Zurich): [arXiv:2410.10347](https://arxiv.org/abs/2410.10347)
- Cascadia (resource-aware cascades): [arXiv:2506.04203](https://arxiv.org/abs/2506.04203)
- STEER (confidence-based stepwise routing): [arXiv:2511.06190](https://arxiv.org/abs/2511.06190)
- SATER (self-aware confidence routing): [arXiv:2510.05164](https://arxiv.org/abs/2510.05164)
- TRIM (step-level routing for reasoning): [Emergent Mind summary](https://www.emergentmind.com/articles/2601.10245)

Related tooling/framework names mentioned:

- RouteLLM
- Martian Router

### Advanced implementation notes worth retaining

- **Calibration matters:** confidence thresholds should be tuned on held-out traffic, not guessed.
- **Per-domain thresholds help:** code/math/legal queries may need different escalation cutoffs.
- **Use multi-signal confidence:** combine logprob, entropy, answer length/structure, and verifier outputs.
- **Measure with policy curves:** track quality vs cloud-call rate to choose operating points.
- **Plan for drift:** retrain or recalibrate routers as prompts, model versions, and traffic mix change.
