import type { OutputItem, StreamUpdate } from "../domain.js";

export interface ChatCompletionToolCall {
  id?: string;
  type?: string;
  function?: {
    name?: string;
    arguments?: string;
  };
}

export interface ChatCompletionMessage {
  role?: string;
  content?: string | null;
  tool_calls?: ChatCompletionToolCall[];
}

export interface ChatCompletionChoice {
  index?: number;
  message?: ChatCompletionMessage;
  delta?: ChatCompletionMessage;
  finish_reason?: string | null;
}

export interface ChatCompletionUsage {
  prompt_tokens?: number;
  completion_tokens?: number;
  total_tokens?: number;
}

export interface ChatCompletionResponse {
  id?: string;
  object?: string;
  choices?: ChatCompletionChoice[];
  usage?: ChatCompletionUsage;
}

export interface AdaptedChatCompletion {
  transport: "chat_completions";
  publicId: string;
  outputItems: OutputItem[];
  streamUpdates: StreamUpdate[];
}

function normalizeToolCall(toolCall: ChatCompletionToolCall): OutputItem {
  return {
    id: toolCall.id ?? `tool-call:${toolCall.function?.name ?? "unknown"}`,
    kind: "tool_call",
    label: ["assistant", "tool_call", toolCall.function?.name].filter(Boolean).join(" / "),
    role: "assistant",
    text: null,
    payload: {
      id: toolCall.id ?? "",
      type: toolCall.type ?? "function",
      function: {
        name: toolCall.function?.name ?? "",
        arguments: toolCall.function?.arguments ?? "",
      },
    },
  };
}

export function normalizeChatCompletionResponse(
  payload: ChatCompletionResponse,
): AdaptedChatCompletion {
  const outputItems: OutputItem[] = [];
  const streamUpdates: StreamUpdate[] = [];

  for (const choice of payload.choices ?? []) {
    if (choice.message?.content) {
      outputItems.push({
        id: `${payload.id ?? "chat"}:message:${choice.index ?? 0}`,
        kind: "assistant_message",
        label: "assistant / assistant_message",
        role: choice.message.role ?? "assistant",
        text: choice.message.content,
        payload: null,
      });
    }

    for (const toolCall of choice.message?.tool_calls ?? []) {
      outputItems.push(normalizeToolCall(toolCall));
    }

    if (choice.finish_reason) {
      streamUpdates.push({
        id: `${payload.id ?? "chat"}:finish:${choice.index ?? 0}`,
        sequenceNumber: choice.index ?? 0,
        kind: "completion",
        label: "Completion",
        phase: "request_complete",
        text: choice.finish_reason,
        payload: payload.usage ?? null,
      });
    }
  }

  return {
    transport: "chat_completions",
    publicId: payload.id ?? "",
    outputItems,
    streamUpdates,
  };
}

export function normalizeChatCompletionChunk(
  payload: ChatCompletionResponse,
): AdaptedChatCompletion {
  const streamUpdates: StreamUpdate[] = [];

  for (const choice of payload.choices ?? []) {
    const delta = choice.delta;
    if (delta?.content) {
      streamUpdates.push({
        id: `${payload.id ?? "chat"}:delta:${choice.index ?? 0}:${streamUpdates.length}`,
        sequenceNumber: choice.index ?? 0,
        kind: "text_delta",
        label: "assistant / output_text",
        phase: "stream_delta",
        text: delta.content,
        payload: null,
      });
    }

    for (const toolCall of delta?.tool_calls ?? []) {
      streamUpdates.push({
        id: `${payload.id ?? "chat"}:tool-delta:${choice.index ?? 0}:${toolCall.id ?? streamUpdates.length}`,
        sequenceNumber: choice.index ?? 0,
        kind: "tool_call_delta",
        label: ["assistant", "tool_call_delta", toolCall.function?.name].filter(Boolean).join(" / "),
        phase: "stream_delta",
        text: null,
        payload: {
          id: toolCall.id ?? "",
          type: toolCall.type ?? "function",
          function: {
            name: toolCall.function?.name ?? "",
            arguments: toolCall.function?.arguments ?? "",
          },
        },
      });
    }

    if (choice.finish_reason) {
      streamUpdates.push({
        id: `${payload.id ?? "chat"}:finish:${choice.index ?? 0}`,
        sequenceNumber: choice.index ?? 0,
        kind: "completion",
        label: "Completion",
        phase: "request_complete",
        text: choice.finish_reason,
        payload: payload.usage ?? null,
      });
    }
  }

  return {
    transport: "chat_completions",
    publicId: payload.id ?? "",
    outputItems: [],
    streamUpdates,
  };
}
