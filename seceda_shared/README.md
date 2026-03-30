# shared

This area is reserved for Seceda-specific contracts, fixtures, and other small
artifacts that may need to be consumed by multiple runtimes.

## Edge-To-Modal SGLang Contract

The current edge runtime calls a deployed Modal-backed SGLang server directly.
The integration target is the OpenAI-compatible surface exposed by
`seceda_cloud/sglang_inference.py`.

### Scope

- Phase 1 is single-turn only.
- The edge HTTP API keeps its current `text` plus optional `system_prompt`
  request shape.
- `CloudClient` may generate a `Modal-Session-ID` for each logical inference
  request when Modal session affinity is enabled.
- Retries for the same logical inference request reuse the same generated
  `Modal-Session-ID`.
- Phase 2 is the multi-turn upgrade path, where callers reuse a stable
  interaction-scoped identifier across related turns.

### Direct Cloud Endpoint

- Base URL: deployed Modal flash URL or another HTTPS base URL that exposes the
  same OpenAI-compatible API.
- Health check: `GET /health`
- Completion endpoint: `POST /v1/chat/completions`
- Non-goal: the edge runtime does not call Modal container-local maintenance
  endpoints such as `/release_memory_occupation` or
  `/resume_memory_occupation`.

### Request Headers

- `Content-Type: application/json`
- `Accept: text/event-stream`
- `Authorization: Bearer <token>` only when the edge runtime is configured with
  a cloud API key
- `Modal-Session-ID: <generated-id>` when Modal session affinity is enabled

### Request Body

The edge runtime sends an OpenAI-style chat-completions payload:

```json
{
  "model": "seceda-cloud-default",
  "messages": [
    {"role": "system", "content": "optional system prompt"},
    {"role": "user", "content": "user text"}
  ],
  "max_tokens": 128,
  "temperature": 0.7,
  "top_p": 0.9,
  "top_k": 40,
  "min_p": 0.05,
  "stream": true
}
```

Forwarding rules:

- `top_k` is only sent when it is positive.
- `min_p` is only sent when it is positive.
- `seed` is only sent when the caller overrides the sentinel default.
- The current edge contract is single-turn, so the edge runtime synthesizes the
  `messages` array from `system_prompt` and `text`.

### Response Shape

The edge runtime expects either:

- Server-sent events with `data: ...` chunks and a terminating `data: [DONE]`
- Or a non-stream JSON response that still follows the OpenAI chat-completions
  shape

For streamed responses, the edge runtime appends `choices[0].delta.content`.
For non-stream fallback JSON responses, it reads `choices[0].message.content`.

### Failure Semantics

- Retryable transport failures: connection and timeout style libcurl errors
- Retryable HTTP failures: `502`, `503`, and `504`
- Non-retryable failures: malformed SSE payloads, missing completion markers,
  and non-retryable HTTP status codes
- Cloud timing should include retry and backoff time for a single logical edge
  request

### Multi-Turn Upgrade Path

The next protocol upgrade should add an optional edge request field such as
`session_id` or `conversation_id`.

Expected Phase 2 behavior:

- callers reuse the same identifier for all turns in one conversation
- the edge runtime forwards that stable identifier as `Modal-Session-ID`
- if the public API evolves beyond `text`, the edge request should move toward a
  first-class `messages` array rather than rebuilding chat state on the server
