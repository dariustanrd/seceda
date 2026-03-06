# Seceda Main Use Cases

This document consolidates the primary use cases for Seceda based on the direction in `README.md`.

Seceda's core value proposition:
- run fast, private inference locally on edge devices with SLMs,
- route difficult requests to stronger cloud LLMs only when needed,
- keep routing decisions on the edge,
- use Modal for elastic cloud execution without always-on GPU servers.

## Core Interaction Pattern

1. A user query arrives on an edge device.
2. A local router evaluates query difficulty or local answer confidence.
3. If confidence is high, return the local SLM answer.
4. If confidence is low, escalate to a cloud LLM through Modal.
5. Return the final answer and track latency, quality, and cost.

## Main Use Cases

### **1. Smart Home / IoT**

**Why this fits:** Home automation queries are often simple and frequent, with occasional planning tasks that need deeper reasoning.

**Local SLM examples (edge-first):**
- "Turn on the living room lights."
- "Set the thermostat to 22C."
- "Is the front door locked?"
- "Start the vacuum in the kitchen."
- "Show the battery level of all door sensors."

**Cloud fallback examples (LLM):**
- "Build a 7-day energy-saving schedule based on our usage patterns."
- "Create an away-mode routine for lights, cameras, and thermostat."
- "Analyze this month's smart meter usage and suggest optimizations."
- "Explain why these automations conflict and propose a clean setup."

**Value:**
- Instant responses for day-to-day controls
- Better privacy for home telemetry
- Cloud usage only for planning and analysis tasks

### **2. In-Vehicle Assistant and Control Copilot**

**Why this fits:** In-car interactions are latency-sensitive, privacy-sensitive, and not always online.

**Local SLM examples (edge-first):**
- "How do I enable lane assist?"
- "Set cabin temperature to 22C."
- "Open charging settings."
- "What does this warning icon mean?"
- "Navigate to the nearest charging station."

**Cloud fallback examples (LLM):**
- "My EV range dropped over 3 weeks. What are likely causes and checks?"
- "Compare these two service plans based on my driving profile."
- "Plan a multi-country EV trip with charging stops and contingencies."
- "Summarize all recurring alerts this month and suggest maintenance priorities."

**Value:**
- Fast in-car response for common tasks
- Better behavior under weak connectivity
- Lower cloud cost per vehicle

### **3. On-Device Assistant for Phones and Laptops**

**Why this fits:** Personal assistants benefit from local privacy and responsiveness while still needing cloud reasoning for complex work.

**Local SLM examples (edge-first):**
- "Summarize this note in 3 bullets."
- "Rewrite this message to sound more professional."
- "Convert this paragraph into a to-do list."
- "Draft a short reply to this email."

**Cloud fallback examples (LLM):**
- "Create a project plan with milestones, owners, and risks."
- "Analyze trade-offs between these architecture options."
- "Write a long-form report from these notes and requirements."
- "Generate multiple audience-specific versions of this document."

**Value:**
- Local-first performance for daily usage
- Sensitive content stays local for most interactions
- Cloud is used selectively for deep reasoning

### **4. Education / Tutoring**

**Why this fits:** Many learning interactions are short and repetitive, but advanced explanations and personalized plans require higher capability.

**Local SLM examples (edge-first):**
- "What is photosynthesis?"
- "Solve 27 x 14."
- "Check this sentence for grammar errors."
- "Quiz me on basic algebra for 5 questions."

**Cloud fallback examples (LLM):**
- "Explain Newton's laws with real-world examples at a high-school level."
- "Give detailed feedback on this 800-word essay."
- "Create a personalized 4-week study plan from these test results."
- "Design a progressive lesson sequence for this student profile."

**Value:**
- Fast tutoring loops on-device
- Better continuity for learning in low-connectivity settings
- High-quality escalation only for deep pedagogy tasks

### **5. Local Coding Agents (Developer Machines)**

**Why this fits:** Developer workflows include many small local edits and checks, with occasional large architectural tasks requiring stronger models.

**Local SLM examples (edge-first):**
- "Explain this stack trace from my local logs."
- "Write unit tests for this function."
- "Refactor this method for readability without changing behavior."
- "Generate commit message options from staged changes."
- "Find likely null checks missing in this file."

**Cloud fallback examples (LLM):**
- "Plan a safe multi-repo migration from REST to gRPC."
- "Design a rollback strategy for this database schema change."
- "Review this full codebase for security and data-leak risks."
- "Propose an architecture for offline-first sync with conflict resolution."

**Value:**
- Private local coding assistance by default
- Lower latency for common edit/test loops
- Cloud escalation for high-context, cross-system reasoning

### **6. Distributed Edge AI Across Many Devices**

**Why this fits:** Seceda is designed for fleets of edge clients where most requests are simple and repetitive.

**Local SLM examples (edge-first):**
- "Where is item A12 in this store?"
- "What time does this branch close?"
- "Show last maintenance time for Machine 4."
- "Restart kiosk app in safe mode."

**Cloud fallback examples (LLM):**
- "Summarize root causes for repeated incidents across all locations."
- "Recommend staffing changes from multi-site demand patterns."
- "Generate an executive summary from fleet-wide logs."
- "Correlate failure patterns between sites and rank top actions."

**Value:**
- Scales without always-on cloud GPUs
- Maintains low average latency across the fleet
- Handles bursty complex traffic with elastic cloud workers

### **7. Latency-Sensitive Command and Voice Interfaces**

**Why this fits:** Command interfaces need near-instant actions, while some requests still need high-reasoning responses.

**Local SLM examples (edge-first):**
- "Mute notifications."
- "Start recording now."
- "Go back to the previous screen."
- "Set focus mode for 45 minutes."

**Cloud fallback examples (LLM):**
- "Draft a policy-compliant incident response with action items."
- "Resolve ambiguous user intent and provide ranked options."
- "Use long conversation context to produce a final recommendation."
- "Generate a multilingual summary for this voice transcript."

**Value:**
- Consistent low-latency UX
- Better reliability under network variance
- Controlled use of expensive cloud inference

### **8. Routing Policy Research and Benchmarking**

**Why this fits:** Seceda is a framework, so experimentation with routing strategies is a first-class use case.

**Local SLM examples (edge-first):**
- Short QA benchmark sets
- Deterministic command datasets
- Template-based prompt suites
- Latency-critical interaction traces

**Cloud fallback examples (LLM):**
- Long-context reasoning benchmarks
- Multi-step planning prompts
- Tool-heavy evaluation cases
- Red-team and robustness test prompts

**Value:**
- Compare routing policies on latency, quality, and cost
- Tune confidence thresholds with measurable impact
- Evaluate cloud model tiering strategies

## Shared Escalation Triggers

Common reasons to escalate from local SLM to cloud LLM:
- low confidence or unstable local outputs,
- long-context or multi-document reasoning needs,
- multi-step planning requirements,
- policy-sensitive responses requiring stronger models,
- tool usage unavailable on-device.

## Suggested MVP Focus

Recommended first implementation scope:
1. Start with one flagship assistant use case (vehicle, smart home, or coding assistant).
2. Implement one simple pluggable routing policy on edge.
3. Add Modal fallback with one cloud model first.
4. Measure core metrics: local-hit rate, cloud-hit rate, latency, and cost per request.

This path best demonstrates Seceda's edge-first goal while keeping the build practical.
