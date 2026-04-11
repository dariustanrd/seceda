export type ExecutionTransport = "chat_completions" | "responses" | "unknown";

export type TraceViewMode = "off" | "prompt" | "debug";

export interface TimingStats {
  totalLatencyMs: number;
  hasTtft: boolean;
  ttftMs: number | null;
  promptTokens: number;
  generatedTokens: number;
  totalTokens: number;
}

export interface RouteIdentity {
  engineId: string;
  backendId: string;
  modelId: string;
  modelAlias: string;
  displayName: string;
  executionMode: string;
}

export interface RawTimingStats {
  total_latency_ms?: number;
  has_ttft?: boolean;
  ttft_ms?: number;
  prompt_tokens?: number;
  generated_tokens?: number;
  total_tokens?: number;
}

export interface RawExecutionEvent {
  id: number;
  request_id: string;
  timestamp_utc: string;
  requested_target: string;
  initial_target: string;
  final_target: string;
  ok: boolean;
  fallback_used?: boolean;
  error_kind?: string;
  error?: string;
  finish_reason?: string;
  route_reason?: string;
  fallback_reason?: string;
  matched_rules?: string[];
  initial_engine_id?: string;
  initial_backend_id?: string;
  initial_model_id?: string;
  initial_model_alias?: string;
  initial_execution_mode?: string;
  engine_id?: string;
  backend_id?: string;
  model_id?: string;
  model_alias?: string;
  display_name?: string;
  execution_mode?: string;
  active_model_path?: string;
  timing?: RawTimingStats;
  local_timing?: RawTimingStats;
  cloud_timing?: RawTimingStats;
  transport?: string;
}

export interface RawTraceEvent {
  id: number;
  sequence_number?: number;
  request_id: string;
  transport?: string;
  timestamp_utc?: string;
  phase?: string;
  item_type?: string;
  item_id?: string;
  content_index?: number | null;
  role?: string;
  tool_name?: string;
  delta_text?: string;
  text?: string;
  payload?: unknown;
  payload_json?: string;
}

export interface ExecutionSummary {
  id: number;
  requestId: string;
  transport: ExecutionTransport;
  timestampUtc: string;
  requestedTarget: string;
  initialTarget: string;
  finalTarget: string;
  ok: boolean;
  fallbackUsed: boolean;
  errorKind: string;
  error: string;
  finishReason: string;
  routeReason: string;
  fallbackReason: string;
  matchedRules: string[];
  initialIdentity: RouteIdentity;
  finalIdentity: RouteIdentity;
  activeModelPath: string;
  timing: TimingStats;
  localTiming: TimingStats | null;
  cloudTiming: TimingStats | null;
}

export interface TraceEvent {
  id: number;
  sequenceNumber: number;
  requestId: string;
  transport: ExecutionTransport;
  timestampUtc: string;
  phase: string;
  itemType: string;
  itemId: string;
  contentIndex: number | null;
  role: string | null;
  toolName: string | null;
  deltaText: string | null;
  text: string | null;
  payload: unknown | null;
}

export interface NormalizedRequestContext {
  model: string;
  routeOverride: string;
  preferredEngineId: string;
  preferredBackendId: string;
  preferredModelAlias: string;
  normalized: Record<string, unknown>;
  capabilities: Record<string, unknown>;
  payload: unknown | null;
}

export interface InputItem {
  id: string;
  kind: string;
  label: string;
  role: string | null;
  text: string | null;
  payload: unknown | null;
}

export interface OutputItem {
  id: string;
  kind: string;
  label: string;
  role: string | null;
  text: string | null;
  payload: unknown | null;
}

export interface StreamUpdate {
  id: string;
  sequenceNumber: number;
  kind: "phase" | "text_delta" | "tool_call_delta" | "completion";
  label: string;
  phase: string | null;
  text: string | null;
  payload: unknown | null;
}

export interface ExecutionDetail {
  summary: ExecutionSummary;
  requestContext: NormalizedRequestContext | null;
  inputItems: InputItem[];
  outputItems: OutputItem[];
  streamUpdates: StreamUpdate[];
  traceEvents: TraceEvent[];
}

export interface ExecutionHistory {
  sinceId: number;
  latestId: number;
  executions: ExecutionSummary[];
}

export interface ExecutionBatch {
  sinceId: number;
  latestId: number;
  executions: ExecutionSummary[];
}

const PROMPT_TRACE_PHASES = new Set([
  "request_normalized",
  "input_item",
  "output_item",
  "request_complete",
  "request_error",
]);

function asRecord(value: unknown): Record<string, unknown> | null {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    return null;
  }
  return value as Record<string, unknown>;
}

function parsePayload(payload: RawTraceEvent): unknown | null {
  if (payload.payload !== undefined) {
    return payload.payload;
  }
  if (!payload.payload_json) {
    return null;
  }

  try {
    return JSON.parse(payload.payload_json);
  } catch {
    return payload.payload_json;
  }
}

export function normalizeTransport(value: string | null | undefined): ExecutionTransport {
  if (!value) {
    return "chat_completions";
  }
  if (value === "chat_completions" || value === "responses") {
    return value;
  }
  return "unknown";
}

export function normalizeTiming(value: RawTimingStats | null | undefined): TimingStats {
  return {
    totalLatencyMs: value?.total_latency_ms ?? 0,
    hasTtft: value?.has_ttft ?? false,
    ttftMs: value?.has_ttft ? (value?.ttft_ms ?? 0) : null,
    promptTokens: value?.prompt_tokens ?? 0,
    generatedTokens: value?.generated_tokens ?? 0,
    totalTokens:
      value?.total_tokens ??
      ((value?.prompt_tokens ?? 0) + (value?.generated_tokens ?? 0)),
  };
}

function normalizeIdentity(raw: {
  engineId?: string;
  backendId?: string;
  modelId?: string;
  modelAlias?: string;
  displayName?: string;
  executionMode?: string;
}): RouteIdentity {
  return {
    engineId: raw.engineId ?? "",
    backendId: raw.backendId ?? "",
    modelId: raw.modelId ?? "",
    modelAlias: raw.modelAlias ?? "",
    displayName: raw.displayName ?? "",
    executionMode: raw.executionMode ?? "",
  };
}

export function toExecutionSummary(event: RawExecutionEvent): ExecutionSummary {
  return {
    id: event.id,
    requestId: event.request_id,
    transport: normalizeTransport(event.transport),
    timestampUtc: event.timestamp_utc,
    requestedTarget: event.requested_target,
    initialTarget: event.initial_target,
    finalTarget: event.final_target,
    ok: event.ok,
    fallbackUsed: event.fallback_used ?? false,
    errorKind: event.error_kind ?? "",
    error: event.error ?? "",
    finishReason: event.finish_reason ?? "",
    routeReason: event.route_reason ?? "",
    fallbackReason: event.fallback_reason ?? "",
    matchedRules: [...(event.matched_rules ?? [])],
    initialIdentity: normalizeIdentity({
      engineId: event.initial_engine_id,
      backendId: event.initial_backend_id,
      modelId: event.initial_model_id,
      modelAlias: event.initial_model_alias,
      executionMode: event.initial_execution_mode,
    }),
    finalIdentity: normalizeIdentity({
      engineId: event.engine_id,
      backendId: event.backend_id,
      modelId: event.model_id,
      modelAlias: event.model_alias,
      displayName: event.display_name,
      executionMode: event.execution_mode,
    }),
    activeModelPath: event.active_model_path ?? "",
    timing: normalizeTiming(event.timing),
    localTiming: event.local_timing ? normalizeTiming(event.local_timing) : null,
    cloudTiming: event.cloud_timing ? normalizeTiming(event.cloud_timing) : null,
  };
}

export function toTraceEvent(event: RawTraceEvent): TraceEvent {
  return {
    id: event.id,
    sequenceNumber: event.sequence_number ?? event.id,
    requestId: event.request_id,
    transport: normalizeTransport(event.transport),
    timestampUtc: event.timestamp_utc ?? "",
    phase: event.phase ?? "",
    itemType: event.item_type ?? "",
    itemId: event.item_id ?? "",
    contentIndex: event.content_index ?? null,
    role: event.role ?? null,
    toolName: event.tool_name ?? null,
    deltaText: event.delta_text ?? null,
    text: event.text ?? null,
    payload: parsePayload(event),
  };
}

export function createExecutionHistory(): ExecutionHistory {
  return {
    sinceId: 0,
    latestId: 0,
    executions: [],
  };
}

export function mergeExecutionHistory(
  history: ExecutionHistory,
  batch: ExecutionBatch,
  maxItems = 250,
): ExecutionHistory {
  const byId = new Map<number, ExecutionSummary>();

  for (const execution of history.executions) {
    byId.set(execution.id, execution);
  }
  for (const execution of batch.executions) {
    byId.set(execution.id, execution);
  }

  const executions = [...byId.values()]
    .sort((left, right) => right.id - left.id)
    .slice(0, maxItems);

  return {
    sinceId: Math.max(history.sinceId, batch.latestId),
    latestId: Math.max(history.latestId, batch.latestId),
    executions,
  };
}

export function filterTraceEvents(
  traceEvents: TraceEvent[],
  mode: TraceViewMode,
): TraceEvent[] {
  if (mode === "off") {
    return [];
  }

  const sorted = [...traceEvents].sort((left, right) => {
    if (left.sequenceNumber !== right.sequenceNumber) {
      return left.sequenceNumber - right.sequenceNumber;
    }
    return left.id - right.id;
  });

  if (mode === "debug") {
    return sorted;
  }

  return sorted.filter((event) => PROMPT_TRACE_PHASES.has(event.phase));
}

function formatPhaseLabel(phase: string): string {
  return phase
    .split("_")
    .filter(Boolean)
    .map((segment) => segment.charAt(0).toUpperCase() + segment.slice(1))
    .join(" ");
}

function formatInputLabel(event: TraceEvent): string {
  const pieces = [event.role, event.itemType].filter(Boolean);
  return pieces.length > 0 ? pieces.join(" / ") : "Input";
}

function formatOutputLabel(event: TraceEvent): string {
  const pieces = [event.role, event.itemType, event.toolName].filter(Boolean);
  return pieces.length > 0 ? pieces.join(" / ") : "Output";
}

function toRequestContext(payload: unknown): NormalizedRequestContext | null {
  const record = asRecord(payload);
  if (!record) {
    return null;
  }

  return {
    model: String(record.model ?? ""),
    routeOverride: String(record.route_override ?? ""),
    preferredEngineId: String(record.preferred_engine_id ?? ""),
    preferredBackendId: String(record.preferred_backend_id ?? ""),
    preferredModelAlias: String(record.preferred_model_alias ?? ""),
    normalized: asRecord(record.normalized) ?? {},
    capabilities: asRecord(record.capabilities) ?? {},
    payload,
  };
}

export function buildExecutionDetail(
  summary: ExecutionSummary,
  traceEvents: TraceEvent[],
  traceMode: TraceViewMode = "debug",
): ExecutionDetail {
  const filteredTraceEvents = filterTraceEvents(traceEvents, traceMode);
  const inputItems: InputItem[] = [];
  const outputItems: OutputItem[] = [];
  const streamUpdates: StreamUpdate[] = [];
  let requestContext: NormalizedRequestContext | null = null;

  for (const event of filteredTraceEvents) {
    switch (event.phase) {
      case "request_normalized":
        requestContext = toRequestContext(event.payload);
        streamUpdates.push({
          id: `phase:${event.id}`,
          sequenceNumber: event.sequenceNumber,
          kind: "phase",
          label: formatPhaseLabel(event.phase),
          phase: event.phase,
          text: null,
          payload: event.payload,
        });
        break;
      case "input_item":
        inputItems.push({
          id: event.itemId || `input:${event.id}`,
          kind: event.itemType || "input_item",
          label: formatInputLabel(event),
          role: event.role,
          text: event.text,
          payload: event.payload,
        });
        break;
      case "output_item":
        outputItems.push({
          id: event.itemId || `output:${event.id}`,
          kind: event.itemType || "output_item",
          label: formatOutputLabel(event),
          role: event.role,
          text: event.text,
          payload: event.payload,
        });
        break;
      case "stream_delta":
        streamUpdates.push({
          id: `delta:${event.id}`,
          sequenceNumber: event.sequenceNumber,
          kind: event.itemType === "tool_call_delta" ? "tool_call_delta" : "text_delta",
          label: formatOutputLabel(event),
          phase: event.phase,
          text: event.deltaText,
          payload: event.payload,
        });
        break;
      default:
        streamUpdates.push({
          id: `phase:${event.id}`,
          sequenceNumber: event.sequenceNumber,
          kind:
            event.phase === "request_complete" || event.phase === "request_error"
              ? "completion"
              : "phase",
          label: formatPhaseLabel(event.phase),
          phase: event.phase,
          text: event.text,
          payload: event.payload,
        });
        break;
    }
  }

  return {
    summary,
    requestContext,
    inputItems,
    outputItems,
    streamUpdates,
    traceEvents: filteredTraceEvents,
  };
}

export function formatLatency(latencyMs: number): string {
  if (!Number.isFinite(latencyMs) || latencyMs <= 0) {
    return "0 ms";
  }
  if (latencyMs >= 1000) {
    return `${(latencyMs / 1000).toFixed(2)} s`;
  }
  return `${latencyMs.toFixed(0)} ms`;
}

export function summarizeExecution(summary: ExecutionSummary): string {
  const status = summary.ok ? "ok" : "err";
  const model =
    summary.finalIdentity.modelAlias ||
    summary.finalIdentity.modelId ||
    summary.initialIdentity.modelAlias ||
    "unknown";
  return `[${status}] ${summary.requestId} ${summary.finalTarget} ${model} ${formatLatency(
    summary.timing.totalLatencyMs,
  )}`;
}

export function describeExecution(summary: ExecutionSummary): string {
  const routing = summary.routeReason || "no route reason";
  const fallback = summary.fallbackUsed ? `fallback: ${summary.fallbackReason || "yes"}` : "no fallback";
  return `${summary.timestampUtc} | ${routing} | ${fallback}`;
}
