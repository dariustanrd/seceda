import type { OutputItem, StreamUpdate } from "../domain.js";

export interface ResponseContentPart {
  type?: string;
  text?: string;
}

export interface ResponseOutputItem {
  id?: string;
  type?: string;
  role?: string;
  content?: ResponseContentPart[];
  name?: string;
  arguments?: string;
  output?: string;
}

export interface ResponsePayload {
  id?: string;
  output?: ResponseOutputItem[];
}

export interface AdaptedResponsePayload {
  transport: "responses";
  publicId: string;
  outputItems: OutputItem[];
  streamUpdates: StreamUpdate[];
}

function normalizeResponseOutputItem(item: ResponseOutputItem, index: number): OutputItem {
  const text = item.content
    ?.map((part) => part.text ?? "")
    .filter(Boolean)
    .join("");

  return {
    id: item.id ?? `response-output:${index}`,
    kind: item.type ?? "response_output_item",
    label: [item.role ?? "assistant", item.type ?? "output"].filter(Boolean).join(" / "),
    role: item.role ?? "assistant",
    text: text || item.output || null,
    payload: item,
  };
}

export function normalizeResponsesPayload(
  payload: ResponsePayload,
): AdaptedResponsePayload {
  return {
    transport: "responses",
    publicId: payload.id ?? "",
    outputItems: (payload.output ?? []).map(normalizeResponseOutputItem),
    streamUpdates: [],
  };
}

export function normalizeResponsesStreamEvent(
  eventType: string,
  payload: Record<string, unknown>,
): StreamUpdate[] {
  switch (eventType) {
    case "response.output_text.delta":
      return [
        {
          id: `${String(payload.response_id ?? "response")}:${String(payload.output_index ?? 0)}:delta`,
          sequenceNumber: Number(payload.output_index ?? 0),
          kind: "text_delta",
          label: "assistant / output_text",
          phase: "stream_delta",
          text: String(payload.delta ?? ""),
          payload,
        },
      ];
    case "response.completed":
      return [
        {
          id: `${String(payload.response_id ?? "response")}:completed`,
          sequenceNumber: Number(payload.output_index ?? 0),
          kind: "completion",
          label: "Completion",
          phase: "request_complete",
          text: String(payload.status ?? "completed"),
          payload,
        },
      ];
    default:
      return [
        {
          id: `${String(payload.response_id ?? "response")}:${eventType}`,
          sequenceNumber: Number(payload.output_index ?? 0),
          kind: "phase",
          label: eventType,
          phase: eventType,
          text: null,
          payload,
        },
      ];
  }
}
