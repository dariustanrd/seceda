import { describe, expect, test } from "bun:test";

import {
  buildExecutionDetail,
  createExecutionHistory,
  mergeExecutionHistory,
  toExecutionSummary,
  toTraceEvent,
} from "../src/lib/domain.js";

describe("domain history", () => {
  test("mergeExecutionHistory appends new executions and keeps newest first", () => {
    const initial = createExecutionHistory();

    const firstBatch = mergeExecutionHistory(initial, {
      sinceId: 0,
      latestId: 2,
      executions: [
        toExecutionSummary({
          id: 1,
          request_id: "req-1",
          timestamp_utc: "2026-04-05T12:00:00Z",
          requested_target: "auto",
          initial_target: "local",
          final_target: "local",
          ok: true,
          timing: { total_latency_ms: 10, prompt_tokens: 2, generated_tokens: 3 },
        }),
        toExecutionSummary({
          id: 2,
          request_id: "req-2",
          timestamp_utc: "2026-04-05T12:00:01Z",
          requested_target: "auto",
          initial_target: "local",
          final_target: "cloud",
          ok: true,
          timing: { total_latency_ms: 20, prompt_tokens: 4, generated_tokens: 5 },
        }),
      ],
    });

    const secondBatch = mergeExecutionHistory(firstBatch, {
      sinceId: 2,
      latestId: 3,
      executions: [
        toExecutionSummary({
          id: 3,
          request_id: "req-3",
          timestamp_utc: "2026-04-05T12:00:02Z",
          requested_target: "auto",
          initial_target: "local",
          final_target: "local",
          ok: false,
          error: "boom",
          timing: { total_latency_ms: 5, prompt_tokens: 1, generated_tokens: 0 },
        }),
      ],
    });

    expect(secondBatch.sinceId).toBe(3);
    expect(secondBatch.executions.map((execution) => execution.requestId)).toEqual([
      "req-3",
      "req-2",
      "req-1",
    ]);
  });
});

describe("execution detail", () => {
  test("buildExecutionDetail normalizes request, inputs, outputs, and deltas", () => {
    const summary = toExecutionSummary({
      id: 10,
      request_id: "chatcmpl-test",
      timestamp_utc: "2026-04-05T12:10:00Z",
      requested_target: "auto",
      initial_target: "local",
      final_target: "local",
      ok: true,
      finish_reason: "stop",
      timing: {
        total_latency_ms: 42,
        has_ttft: true,
        ttft_ms: 10,
        prompt_tokens: 8,
        generated_tokens: 5,
      },
    });

    const detail = buildExecutionDetail(
      summary,
      [
        toTraceEvent({
          id: 1,
          request_id: "chatcmpl-test",
          sequence_number: 1,
          phase: "request_normalized",
          item_type: "request",
          item_id: "request",
          payload: {
            model: "seceda/default",
            route_override: "auto",
            normalized: {
              system_prompt: "You are Seceda.",
              latest_user_message: "hello",
            },
          },
        }),
        toTraceEvent({
          id: 2,
          request_id: "chatcmpl-test",
          sequence_number: 2,
          phase: "input_item",
          item_type: "message",
          item_id: "input-0",
          role: "user",
          text: "hello",
        }),
        toTraceEvent({
          id: 3,
          request_id: "chatcmpl-test",
          sequence_number: 3,
          phase: "stream_delta",
          item_type: "output_text",
          item_id: "assistant-output",
          role: "assistant",
          delta_text: "hel",
        }),
        toTraceEvent({
          id: 4,
          request_id: "chatcmpl-test",
          sequence_number: 4,
          phase: "output_item",
          item_type: "assistant_message",
          item_id: "assistant-output",
          role: "assistant",
          text: "hello from local",
        }),
        toTraceEvent({
          id: 5,
          request_id: "chatcmpl-test",
          sequence_number: 5,
          phase: "request_complete",
          item_type: "request",
          item_id: "request",
        }),
      ],
      "debug",
    );

    expect(detail.requestContext?.model).toBe("seceda/default");
    expect(detail.inputItems).toHaveLength(1);
    expect(detail.outputItems).toHaveLength(1);
    expect(detail.outputItems[0]?.text).toBe("hello from local");
    expect(detail.streamUpdates.some((update) => update.kind === "text_delta")).toBe(
      true,
    );
    expect(
      detail.streamUpdates.some((update) => update.kind === "completion"),
    ).toBe(true);
  });
});
