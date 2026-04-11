import { describe, expect, test } from "bun:test";

import {
  normalizeChatCompletionChunk,
  normalizeChatCompletionResponse,
} from "../src/lib/transport_adapters/chat_completions.js";
import {
  normalizeResponsesPayload,
  normalizeResponsesStreamEvent,
} from "../src/lib/transport_adapters/responses.js";

describe("transport adapters", () => {
  test("chat completions normalize into generic output items and updates", () => {
    const completion = normalizeChatCompletionResponse({
      id: "chatcmpl-123",
      choices: [
        {
          index: 0,
          message: {
            role: "assistant",
            content: "hello",
            tool_calls: [
              {
                id: "tool-1",
                function: {
                  name: "lookup_weather",
                  arguments: "{\"city\":\"Berlin\"}",
                },
              },
            ],
          },
          finish_reason: "stop",
        },
      ],
    });

    const chunk = normalizeChatCompletionChunk({
      id: "chatcmpl-123",
      choices: [
        {
          index: 0,
          delta: {
            content: "hel",
          },
        },
      ],
    });

    expect(completion.transport).toBe("chat_completions");
    expect(completion.outputItems).toHaveLength(2);
    expect(completion.streamUpdates[0]?.kind).toBe("completion");
    expect(chunk.streamUpdates[0]?.kind).toBe("text_delta");
    expect(chunk.streamUpdates[0]?.text).toBe("hel");
  });

  test("responses normalize into the same generic output and stream shape", () => {
    const response = normalizeResponsesPayload({
      id: "resp_123",
      output: [
        {
          id: "item-1",
          type: "message",
          role: "assistant",
          content: [{ type: "output_text", text: "hello" }],
        },
      ],
    });

    const event = normalizeResponsesStreamEvent("response.output_text.delta", {
      response_id: "resp_123",
      output_index: 0,
      delta: "hel",
    });

    expect(response.transport).toBe("responses");
    expect(response.outputItems[0]?.text).toBe("hello");
    expect(event[0]?.kind).toBe("text_delta");
    expect(event[0]?.text).toBe("hel");
  });
});
