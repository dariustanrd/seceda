# Codex + Seceda Responses API Overview

## Purpose

Seceda should support a user running Codex through Seceda as a local OpenAI-compatible endpoint.

The immediate product truth is:

- Seceda is first an OpenAI-compatible localhost edge gateway for Codex and similar clients.
- Seceda's broader hybrid local/cloud routing framework remains the internal engine and extensibility story.
- Codex compatibility is the primary external acceptance target for `POST /v1/responses`.

This document is intentionally high level. It captures the restart assumptions for rebuilding the feature in another directory or fresh codebase.

## Desired User Flow

The user should be able to configure Codex to point at Seceda instead of the hosted OpenAI API. Codex should then use Seceda's `POST /v1/responses` endpoint normally, including SSE streaming and tool/function-call flows.

At runtime:

1. Codex sends a Responses API request to Seceda.
2. Seceda accepts and validates the request using OpenAI-compatible wire semantics.
3. Seceda normalizes the request into its internal inference contract.
4. Seceda routes execution across local, sidecar, or cloud backends according to Seceda policy.
5. Seceda converts the internal result or stream back into Responses API JSON or SSE events.
6. Codex receives a response shape it can consume without Seceda-specific coupling.

Seceda-specific metadata is tracked separately by Seceda itself. Codex does not need to see routing decisions, backend identity, timing details, or Seceda-native diagnostics inline in the Responses payload.

## API Boundary

The public compatibility surface should be OpenAI-compatible:

- `POST /v1/responses` is the required endpoint for Codex support.
- SSE streaming must be supported.
- Response objects and `response.*` stream events should follow the OpenAI Responses API shape closely enough for a real Codex local run to work.
- Unsupported behavior should return OpenAI-style errors, not raw internal errors.

The internal Seceda runtime should remain Seceda-owned:

- The external Responses API should be a transport adapter, not the core internal data model.
- Seceda's internal contract may be a superset of OpenAI semantics where useful for routing, backend selection, observability, fallback, model identity, and local/cloud execution.
- Native Responses concepts should enter the internal contract only when Codex-required behavior cannot be represented safely through the existing or planned Seceda inference model.

The key design principle is: OpenAI-compatible at the edge, Seceda-specific inside.

## Compatibility Target

The compatibility oracle is an actual local Codex run against Seceda.

Unit and integration tests are useful, but the feature is not done until Codex can complete its normal loop against Seceda. The test ladder should be:

1. Parser and serializer tests for Responses request/response shapes.
2. HTTP integration tests for non-streaming and streaming `POST /v1/responses`.
3. Tool/function-call flow tests, including streamed function-call arguments and function-call outputs.
4. A real Codex local smoke test pointed at Seceda.

The real Codex run is the final arbiter because Codex may depend on exact event ordering, required fields, or tolerant parsing behavior that smaller tests miss.

## Feature Scope

Seceda should aim to support most or all Responses API features Codex uses.

Day-one Codex support should include:

- Text input and output.
- `instructions`.
- Conversation history represented through Responses `input` items.
- Assistant output items.
- Function calls.
- Function-call outputs.
- Streamed text deltas.
- Streamed function-call argument deltas.
- Reasoning-related fields that Codex sends or expects, to the extent Seceda can preserve or pass them through.
- Explicit errors for features that are not yet implemented.

Seceda should reject unsupported semantic features explicitly instead of pretending to support them. This includes any field or tool type that would change execution semantics and cannot be honored yet.

Seceda may tolerate harmless client bookkeeping fields when ignoring them does not change behavior. Examples include optional item IDs, statuses, metadata, or `store: false` style declarations when Seceda is operating statelessly.

## Streaming Requirements

SSE support is mandatory.

Codex depends heavily on streaming behavior, so streaming should not be treated as a later polish layer. The implementation needs a proper Responses stream adapter with stable stream state.

The stream adapter should maintain:

- Stable response IDs.
- Stable output item IDs.
- Stable function-call item IDs.
- `output_index`.
- `content_index`.
- Monotonic `sequence_number`.
- Correct event ordering.

The expected text stream shape should include the usual lifecycle:

- `response.created`
- `response.in_progress`
- `response.output_item.added`
- `response.content_part.added`
- `response.output_text.delta`
- `response.output_text.done`
- `response.content_part.done`
- `response.output_item.done`
- `response.completed`

For function calls, Seceda also needs function-call item events and argument delta/done events in the shape Codex expects.

## Fallback Policy

Fallback should follow the same core safety rule as chat streaming:

- Before the first user-visible streamed content or tool-call delta, Seceda may fallback to another backend.
- After the first user-visible streamed content or tool-call delta, Seceda should stay on the selected backend and surface errors in-stream.

This avoids producing incoherent mixed-backend streams while still allowing pre-first-byte routing recovery.

Responses event ordering is more complex than chat completions, so the implementation should treat "first user-visible output" carefully. Lifecycle events such as `response.created` should not necessarily freeze fallback if no actual model output has been emitted yet. Text deltas and function-call argument deltas should.

## State And Persistence

The initial implementation should not invent fake persistence.

If Codex sends full history in the request, Seceda can operate statelessly. If Codex sends stateful fields that require server-side memory, Seceda should either implement that state correctly or reject the request with an OpenAI-style `invalid_request_error`.

Recommended default:

- Support stateless operation where the client provides the necessary context.
- Reject `previous_response_id`, server-side conversations, background jobs, retrieval, cancellation, and related stored-response endpoints until Seceda has a real state model.
- Return response fields honestly. For example, do not claim stored behavior if no retrieval path exists.

## Internal Architecture

The preferred architecture is a Responses transport adapter around a Seceda-owned inference runtime.

High-level components:

```text
Codex
  -> POST /v1/responses
  -> Responses request parser / validator
  -> Responses-to-Seceda normalizer
  -> Seceda inference request
  -> Router
  -> Local / sidecar / cloud executor
  -> Seceda inference response or stream events
  -> Seceda-to-Responses serializer
  -> Response JSON or SSE stream
  -> Codex
```

The transport adapter should own:

- OpenAI wire validation.
- Responses request parsing.
- Mapping Responses input items into Seceda messages/tool context.
- Mapping Seceda output into Responses output items.
- Responses SSE event sequencing.
- OpenAI-style error objects for unsupported or invalid request shapes.

The runtime should own:

- Model/backend selection.
- Local vs cloud routing.
- Fallback decisions.
- Execution contracts.
- Backend identity.
- Seceda observability.
- Internal metadata.

Avoid leaking Responses-specific wire details into routing or backend adapters unless there is a real execution need.

## Observability

Seceda observability is separate from Codex compatibility.

Codex should receive clean Responses payloads. Seceda should separately track:

- Request ID.
- Route decision.
- Backend ID.
- Engine ID.
- Model alias.
- Execution mode.
- Timing.
- Token usage when available.
- Fallback behavior.
- Error cause.

There does not need to be a Codex-visible linkage beyond ordinary request IDs and Seceda's own tracking mechanisms.

## Error Philosophy

Unsupported features should fail explicitly.

Recommended behavior:

- Return OpenAI-style `invalid_request_error` for unsupported request fields or tool types that Seceda cannot honor.
- Include a clear message naming the unsupported feature.
- Avoid silently dropping fields that affect semantics.
- Tolerate harmless bookkeeping fields only when ignoring them cannot change model behavior.

This keeps compatibility honest and makes failures actionable during Codex smoke testing.

## Implementation Priorities

1. Implement the `/v1/responses` route and request parser.
2. Normalize Codex-relevant Responses requests into Seceda's internal request contract.
3. Implement non-streaming Responses response serialization.
4. Implement SSE streaming with correct Responses event order and sequence state.
5. Support function calls, function-call outputs, and streamed function-call arguments.
6. Add explicit OpenAI-style errors for unsupported stateful, multimodal, or tool features.
7. Run a real local Codex smoke test against Seceda and use that as the compatibility gate.
8. Expand support for additional Codex-used Responses fields as the smoke test reveals them.

## Non-Goals For The First Restart

- Do not redesign Seceda around native OpenAI Responses objects.
- Do not make Codex aware of Seceda routing metadata.
- Do not fake persistence for `previous_response_id` or stored responses.
- Do not broaden the API surface beyond what Codex needs unless the implementation is already cleanly available.
- Do not treat passing schema-shaped unit tests as sufficient without a real Codex run.

## Summary

Seceda should act like an OpenAI-compatible Responses API server to Codex while preserving a Seceda-specific internal runtime for routing and hybrid execution.

The right first architecture is a strict, streaming-capable Responses transport adapter over a Seceda-owned inference contract. Support the full set of Responses features Codex actually uses, reject unsupported semantic features explicitly, keep Seceda metadata separate, and make a real local Codex run the final definition of done.
