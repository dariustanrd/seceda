import {
  buildExecutionDetail,
  mergeExecutionHistory,
  toExecutionSummary,
  toTraceEvent,
  type ExecutionBatch,
  type ExecutionDetail,
  type ExecutionHistory,
  type RawExecutionEvent,
  type RawTraceEvent,
  type TraceEvent,
  type TraceViewMode,
} from "./domain.js";
import { DEFAULT_BASE_URL } from "./paths.js";

interface EventBatchResponse<TEvent> {
  since_id: number;
  latest_id: number;
  returned: number;
  request_id?: string;
  events: TEvent[];
}

export interface MetricSample {
  name: string;
  labels: Record<string, string>;
  value: number;
}

export interface MetricsSnapshot {
  rawText: string;
  samples: MetricSample[];
}

export interface ModelCatalogEntry {
  id: string;
  object?: string;
  owned_by?: string;
}

export interface ModelsListResponse {
  object?: string;
  data?: ModelCatalogEntry[];
}

export interface InfoSnapshot {
  state?: string;
  host?: string;
  port?: number;
  default_system_prompt?: string;
  public_model_alias?: string;
  last_error?: string;
  local_model?: Record<string, unknown>;
  cloud_client?: Record<string, unknown>;
}

export interface ReloadModelResponse {
  ok: boolean;
  active_model_path?: string;
  error?: string;
}

function encodeQuery(params: Record<string, string | number | undefined>): string {
  const query = new URLSearchParams();

  for (const [key, value] of Object.entries(params)) {
    if (value === undefined || value === "") {
      continue;
    }
    query.set(key, String(value));
  }

  const encoded = query.toString();
  return encoded ? `?${encoded}` : "";
}

function parseMetricLabels(value: string | undefined): Record<string, string> {
  if (!value) {
    return {};
  }

  return value.split(",").reduce<Record<string, string>>((labels, pair) => {
    const match = pair.match(/^\s*([^=]+)="(.*)"\s*$/);
    if (!match) {
      return labels;
    }
    labels[match[1]] = match[2].replaceAll('\\"', '"').replaceAll("\\\\", "\\");
    return labels;
  }, {});
}

export function parsePrometheusMetrics(text: string): MetricsSnapshot {
  const metricLine = /^([a-zA-Z_:][a-zA-Z0-9_:]*)(?:\{([^}]*)\})?\s+(-?\d+(?:\.\d+)?)$/;
  const samples: MetricSample[] = [];

  for (const line of text.split("\n")) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith("#")) {
      continue;
    }

    const match = trimmed.match(metricLine);
    if (!match) {
      continue;
    }

    samples.push({
      name: match[1],
      labels: parseMetricLabels(match[2]),
      value: Number.parseFloat(match[3]),
    });
  }

  return {
    rawText: text,
    samples,
  };
}

export function getMetricValue(
  snapshot: MetricsSnapshot,
  name: string,
  labels: Record<string, string> = {},
): number | null {
  for (const sample of snapshot.samples) {
    if (sample.name !== name) {
      continue;
    }

    const matches = Object.entries(labels).every(
      ([key, value]) => sample.labels[key] === value,
    );
    if (matches) {
      return sample.value;
    }
  }

  return null;
}

export class SecedaApi {
  readonly baseUrl: string;

  constructor(baseUrl = DEFAULT_BASE_URL) {
    this.baseUrl = baseUrl;
  }

  private async fetchJson<T>(path: string, init?: RequestInit): Promise<T> {
    const response = await fetch(`${this.baseUrl}${path}`, {
      ...init,
      headers: {
        "content-type": "application/json",
        ...(init?.headers ?? {}),
      },
    });

    if (!response.ok) {
      throw new Error(`${path} failed with HTTP ${response.status}`);
    }

    return (await response.json()) as T;
  }

  private async fetchText(path: string): Promise<string> {
    const response = await fetch(`${this.baseUrl}${path}`);
    if (!response.ok) {
      throw new Error(`${path} failed with HTTP ${response.status}`);
    }
    return response.text();
  }

  async fetchInfo(): Promise<InfoSnapshot> {
    return this.fetchJson<InfoSnapshot>("/info");
  }

  async fetchModels(): Promise<ModelCatalogEntry[]> {
    const payload = await this.fetchJson<ModelsListResponse>("/v1/models");
    return payload.data ?? [];
  }

  async fetchMetrics(): Promise<MetricsSnapshot> {
    return parsePrometheusMetrics(await this.fetchText("/metrics"));
  }

  async fetchEvents(
    sinceId = 0,
    limit = 100,
  ): Promise<ExecutionBatch> {
    const payload = await this.fetchJson<EventBatchResponse<RawExecutionEvent>>(
      `/metrics/events${encodeQuery({ since_id: sinceId, limit })}`,
    );

    return {
      sinceId: payload.since_id,
      latestId: payload.latest_id,
      executions: payload.events.map(toExecutionSummary),
    };
  }

  async fetchExecutionById(requestId: string): Promise<ExecutionDetail | null> {
    const payload = await this.fetchJson<EventBatchResponse<RawExecutionEvent>>(
      `/metrics/events${encodeQuery({ request_id: requestId, limit: 1 })}`,
    );
    if (payload.events.length === 0) {
      return null;
    }

    return buildExecutionDetail(toExecutionSummary(payload.events[0]), []);
  }

  async fetchTraceEvents(
    requestId: string,
    sinceId = 0,
    limit = 2000,
  ): Promise<TraceEvent[]> {
    const payload = await this.fetchJson<EventBatchResponse<RawTraceEvent>>(
      `/trace/events${encodeQuery({
        request_id: requestId,
        since_id: sinceId,
        limit,
      })}`,
    );

    return payload.events.map(toTraceEvent);
  }

  async fetchExecutionDetail(
    requestId: string,
    traceMode: TraceViewMode,
  ): Promise<ExecutionDetail | null> {
    const payload = await this.fetchJson<EventBatchResponse<RawExecutionEvent>>(
      `/metrics/events${encodeQuery({ request_id: requestId, limit: 1 })}`,
    );

    if (payload.events.length === 0) {
      return null;
    }

    const summary = toExecutionSummary(payload.events[0]);
    const traceEvents =
      traceMode === "off" ? [] : await this.fetchTraceEvents(requestId);

    return buildExecutionDetail(summary, traceEvents, traceMode);
  }

  async reloadModel(
    modelPath: string,
    warmupPrompt = "",
  ): Promise<ReloadModelResponse> {
    return this.fetchJson<ReloadModelResponse>("/admin/model", {
      method: "POST",
      body: JSON.stringify({
        model_path: modelPath,
        warmup_prompt: warmupPrompt,
      }),
    });
  }
}

export async function pollExecutionHistory(
  api: SecedaApi,
  history: ExecutionHistory,
  limit = 100,
): Promise<ExecutionHistory> {
  const batch = await api.fetchEvents(history.sinceId, limit);
  if (batch.executions.length === 0 && batch.latestId === history.latestId) {
    return history;
  }
  return mergeExecutionHistory(history, batch);
}
