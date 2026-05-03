import { afterEach, describe, expect, mock, test } from "bun:test";

import { parsePrometheusMetrics, SecedaApi } from "../src/lib/seceda_api.js";

const originalFetch = globalThis.fetch;

afterEach(() => {
  globalThis.fetch = originalFetch;
});

describe("seceda api", () => {
  test("fetchExecutionDetail joins metrics events and trace events by request id", async () => {
    const calls: string[] = [];

    globalThis.fetch = mock(async (input: RequestInfo | URL) => {
      const url = String(input);
      calls.push(url);

      if (url.endsWith("/metrics/events?request_id=req-1&limit=1")) {
        return new Response(
          JSON.stringify({
            since_id: 0,
            latest_id: 1,
            returned: 1,
            events: [
              {
                id: 1,
                request_id: "req-1",
                timestamp_utc: "2026-04-05T12:00:00Z",
                requested_target: "auto",
                initial_target: "local",
                final_target: "local",
                ok: true,
                finish_reason: "stop",
                timing: {
                  total_latency_ms: 20,
                  prompt_tokens: 3,
                  generated_tokens: 4,
                },
              },
            ],
          }),
        );
      }

      if (url.endsWith("/trace/events?request_id=req-1&since_id=0&limit=2000")) {
        return new Response(
          JSON.stringify({
            since_id: 0,
            latest_id: 3,
            returned: 3,
            events: [
              {
                id: 1,
                request_id: "req-1",
                sequence_number: 1,
                phase: "request_normalized",
                item_type: "request",
                item_id: "request",
                payload: {
                  model: "seceda/default",
                },
              },
              {
                id: 2,
                request_id: "req-1",
                sequence_number: 2,
                phase: "input_item",
                item_type: "message",
                item_id: "input-0",
                role: "user",
                text: "hello",
              },
              {
                id: 3,
                request_id: "req-1",
                sequence_number: 3,
                phase: "output_item",
                item_type: "assistant_message",
                item_id: "assistant-output",
                role: "assistant",
                text: "hello back",
              },
            ],
          }),
        );
      }

      return new Response("not found", { status: 404 });
    }) as unknown as typeof fetch;

    const api = new SecedaApi("http://example.test");
    const detail = await api.fetchExecutionDetail("req-1", "prompt");

    expect(detail?.summary.requestId).toBe("req-1");
    expect(detail?.requestContext?.model).toBe("seceda/default");
    expect(detail?.inputItems[0]?.text).toBe("hello");
    expect(detail?.outputItems[0]?.text).toBe("hello back");
    expect(calls).toEqual([
      "http://example.test/metrics/events?request_id=req-1&limit=1",
      "http://example.test/trace/events?request_id=req-1&since_id=0&limit=2000",
    ]);
  });

  test("parsePrometheusMetrics keeps labeled and unlabeled counters", () => {
    const parsed = parsePrometheusMetrics(`
# HELP seceda_edge_requests_total Total chat completion requests
seceda_edge_requests_total 12
seceda_edge_requests_by_target_total{target="local"} 7
seceda_edge_requests_by_target_total{target="cloud"} 5
`);

    expect(parsed.samples).toHaveLength(3);
    expect(parsed.samples[0]).toEqual({
      name: "seceda_edge_requests_total",
      labels: {},
      value: 12,
    });
    expect(parsed.samples[1]?.labels.target).toBe("local");
  });
});
