import type { ScrollBoxRenderable, SelectOption } from "@opentui/core";
import { useKeyboard } from "@opentui/react";
import { useEffect, useMemo, useRef, useState } from "react";

import {
  createExecutionHistory,
  describeExecution,
  formatLatency,
  mergeExecutionHistory,
  summarizeExecution,
  type ExecutionDetail,
  type ExecutionHistory,
  type TraceViewMode,
} from "../lib/domain.js";
import {
  getMetricValue,
  type InfoSnapshot,
  type MetricSample,
  type MetricsSnapshot,
  type ModelCatalogEntry,
  type SecedaApi,
} from "../lib/seceda_api.js";

interface ObservabilityScreenProps {
  api: SecedaApi;
  active: boolean;
}

const HISTORY_POLL_MS = 1200;
const DETAIL_POLL_MS = 1500;
const CONTEXT_POLL_MS = 4500;

function clampIndex(index: number, length: number): number {
  if (length <= 0) {
    return 0;
  }
  return Math.max(0, Math.min(index, length - 1));
}

function prettyValue(value: unknown): string {
  if (value === null || value === undefined) {
    return "";
  }
  if (typeof value === "string") {
    return value;
  }
  return JSON.stringify(value, null, 2);
}

function buildMetricLines(metrics: MetricsSnapshot | null): string[] {
  if (!metrics) {
    return ["No metrics yet."];
  }

  const requestsTotal = getMetricValue(metrics, "seceda_edge_requests_total") ?? 0;
  const errorsTotal = getMetricValue(metrics, "seceda_edge_errors_total") ?? 0;
  const fallbackTotal =
    getMetricValue(metrics, "seceda_edge_fallback_requests_total") ?? 0;
  const localTotal =
    getMetricValue(metrics, "seceda_edge_requests_by_target_total", { target: "local" }) ??
    0;
  const cloudTotal =
    getMetricValue(metrics, "seceda_edge_requests_by_target_total", { target: "cloud" }) ??
    0;
  const inflight = getMetricValue(metrics, "seceda_edge_requests_in_flight") ?? 0;

  return [
    `Requests: ${requestsTotal}`,
    `Errors: ${errorsTotal}`,
    `Fallbacks: ${fallbackTotal}`,
    `Local: ${localTotal}`,
    `Cloud: ${cloudTotal}`,
    `In flight: ${inflight}`,
  ];
}

function buildTopMetricSamples(metrics: MetricsSnapshot | null): MetricSample[] {
  if (!metrics) {
    return [];
  }

  return metrics.samples
    .filter((sample) => sample.name.startsWith("seceda_edge_"))
    .slice(0, 8);
}

function buildDetailLines(
  detail: ExecutionDetail | null,
  info: InfoSnapshot | null,
  models: ModelCatalogEntry[],
  traceMode: TraceViewMode,
): string[] {
  if (!detail) {
    return ["Select a request to load its detail view."];
  }

  const summary = detail.summary;
  const lines = [
    `request_id: ${summary.requestId}`,
    `transport: ${summary.transport}`,
    `status: ${summary.ok ? "ok" : "error"}`,
    `timestamp: ${summary.timestampUtc}`,
    `targets: ${summary.requestedTarget} -> ${summary.initialTarget} -> ${summary.finalTarget}`,
    `route_reason: ${summary.routeReason || "-"}`,
    `fallback: ${summary.fallbackUsed ? summary.fallbackReason || "yes" : "no"}`,
    `finish_reason: ${summary.finishReason || "-"}`,
    `matched_rules: ${summary.matchedRules.join(", ") || "-"}`,
    `active_model_path: ${summary.activeModelPath || "-"}`,
    `total_latency: ${formatLatency(summary.timing.totalLatencyMs)}`,
    `ttft: ${
      summary.timing.ttftMs === null ? "-" : formatLatency(summary.timing.ttftMs)
    }`,
    `tokens: prompt ${summary.timing.promptTokens}, generated ${summary.timing.generatedTokens}, total ${summary.timing.totalTokens}`,
    "",
    "Final identity",
    `  engine: ${summary.finalIdentity.engineId || "-"}`,
    `  backend: ${summary.finalIdentity.backendId || "-"}`,
    `  model: ${summary.finalIdentity.modelAlias || summary.finalIdentity.modelId || "-"}`,
    `  mode: ${summary.finalIdentity.executionMode || "-"}`,
    "",
  ];

  if (detail.requestContext) {
    lines.push("Normalized request");
    lines.push(`  model: ${detail.requestContext.model || "-"}`);
    lines.push(`  route_override: ${detail.requestContext.routeOverride || "auto"}`);
    if (detail.requestContext.preferredEngineId) {
      lines.push(`  preferred_engine_id: ${detail.requestContext.preferredEngineId}`);
    }
    if (detail.requestContext.preferredBackendId) {
      lines.push(`  preferred_backend_id: ${detail.requestContext.preferredBackendId}`);
    }
    if (detail.requestContext.preferredModelAlias) {
      lines.push(`  preferred_model_alias: ${detail.requestContext.preferredModelAlias}`);
    }
    if (Object.keys(detail.requestContext.normalized).length > 0) {
      lines.push(`  normalized: ${prettyValue(detail.requestContext.normalized)}`);
    }
    lines.push("");
  }

  lines.push("Input items");
  if (detail.inputItems.length === 0) {
    lines.push("  none");
  } else {
    for (const item of detail.inputItems) {
      lines.push(`  - ${item.label}`);
      if (item.text) {
        lines.push(`    text: ${item.text}`);
      }
      if (item.payload) {
        lines.push(`    payload: ${prettyValue(item.payload)}`);
      }
    }
  }
  lines.push("");

  lines.push("Output items");
  if (detail.outputItems.length === 0) {
    lines.push("  none");
  } else {
    for (const item of detail.outputItems) {
      lines.push(`  - ${item.label}`);
      if (item.text) {
        lines.push(`    text: ${item.text}`);
      }
      if (item.payload) {
        lines.push(`    payload: ${prettyValue(item.payload)}`);
      }
    }
  }
  lines.push("");

  lines.push(`Trace mode: ${traceMode}`);
  if (traceMode === "off") {
    lines.push("  Trace fetch disabled. Press 2 or 3 to enable prompt or debug traces.");
  } else if (detail.streamUpdates.length === 0) {
    lines.push("  No stream updates captured.");
  } else {
    for (const update of detail.streamUpdates.slice(-24)) {
      const value = update.text ? ` ${update.text}` : "";
      lines.push(`  - ${update.label}${value}`);
      if (update.payload && update.kind !== "text_delta") {
        lines.push(`    payload: ${prettyValue(update.payload)}`);
      }
    }
  }
  lines.push("");

  lines.push("Daemon context");
  lines.push(`  state: ${info?.state ?? "-"}`);
  lines.push(`  bind: ${info?.host ?? "-"}:${info?.port ?? "-"}`);
  lines.push(`  public_model_alias: ${info?.public_model_alias ?? "-"}`);
  lines.push(`  exposed_models: ${models.map((model) => model.id).join(", ") || "-"}`);
  lines.push(`  last_error: ${info?.last_error ?? "-"}`);

  return lines;
}

export function ObservabilityScreen({
  api,
  active,
}: ObservabilityScreenProps) {
  const [history, setHistory] = useState<ExecutionHistory>(createExecutionHistory);
  const [selectedIndex, setSelectedIndex] = useState(0);
  const [selectedRequestId, setSelectedRequestId] = useState("");
  const [detail, setDetail] = useState<ExecutionDetail | null>(null);
  const [traceMode, setTraceMode] = useState<TraceViewMode>("off");
  const [metrics, setMetrics] = useState<MetricsSnapshot | null>(null);
  const [info, setInfo] = useState<InfoSnapshot | null>(null);
  const [models, setModels] = useState<ModelCatalogEntry[]>([]);
  const [statusLine, setStatusLine] = useState("Waiting for first poll...");
  const detailRef = useRef<ScrollBoxRenderable | null>(null);
  const historyRef = useRef(history);

  historyRef.current = history;

  const requestOptions = useMemo<SelectOption[]>(
    () =>
      history.executions.map((execution) => ({
        name: summarizeExecution(execution),
        description: describeExecution(execution),
        value: execution.requestId,
      })),
    [history.executions],
  );

  const summaryLines = useMemo(() => buildMetricLines(metrics), [metrics]);
  const detailLines = useMemo(
    () => buildDetailLines(detail, info, models, traceMode),
    [detail, info, models, traceMode],
  );
  const topSamples = useMemo(() => buildTopMetricSamples(metrics), [metrics]);

  useEffect(() => {
    const nextIndex = history.executions.findIndex(
      (execution) => execution.requestId === selectedRequestId,
    );

    if (history.executions.length === 0) {
      setSelectedIndex(0);
      setSelectedRequestId("");
      return;
    }

    if (nextIndex === -1) {
      setSelectedIndex(0);
      setSelectedRequestId(history.executions[0].requestId);
      return;
    }

    setSelectedIndex(clampIndex(nextIndex, history.executions.length));
  }, [history.executions, selectedRequestId]);

  useEffect(() => {
    if (!active) {
      return;
    }

    let cancelled = false;
    let timer: ReturnType<typeof setTimeout> | undefined;

    const poll = async () => {
      try {
        const batch = await api.fetchEvents(historyRef.current.sinceId, 100);
        if (!cancelled) {
          setHistory((current) => mergeExecutionHistory(current, batch));
          if (batch.executions.length > 0) {
            setStatusLine(`History updated through event ${batch.latestId}.`);
          }
        }
      } catch (error) {
        if (!cancelled) {
          setStatusLine(`History poll failed: ${String(error)}`);
        }
      } finally {
        if (!cancelled) {
          timer = setTimeout(poll, HISTORY_POLL_MS);
        }
      }
    };

    void poll();

    return () => {
      cancelled = true;
      if (timer) {
        clearTimeout(timer);
      }
    };
  }, [active, api]);

  useEffect(() => {
    if (!active) {
      return;
    }

    let cancelled = false;
    let timer: ReturnType<typeof setTimeout> | undefined;

    const refresh = async () => {
      try {
        const [nextInfo, nextModels, nextMetrics] = await Promise.all([
          api.fetchInfo(),
          api.fetchModels(),
          api.fetchMetrics(),
        ]);

        if (!cancelled) {
          setInfo(nextInfo);
          setModels(nextModels);
          setMetrics(nextMetrics);
        }
      } catch (error) {
        if (!cancelled) {
          setStatusLine(`Context refresh failed: ${String(error)}`);
        }
      } finally {
        if (!cancelled) {
          timer = setTimeout(refresh, CONTEXT_POLL_MS);
        }
      }
    };

    void refresh();

    return () => {
      cancelled = true;
      if (timer) {
        clearTimeout(timer);
      }
    };
  }, [active, api]);

  useEffect(() => {
    if (!active || !selectedRequestId) {
      return;
    }

    let cancelled = false;
    let timer: ReturnType<typeof setTimeout> | undefined;

    const refresh = async () => {
      try {
        const nextDetail = await api.fetchExecutionDetail(selectedRequestId, traceMode);
        if (!cancelled) {
          setDetail(nextDetail);
        }
      } catch (error) {
        if (!cancelled) {
          setStatusLine(`Detail lookup failed: ${String(error)}`);
        }
      } finally {
        if (!cancelled) {
          timer = setTimeout(refresh, DETAIL_POLL_MS);
        }
      }
    };

    void refresh();

    return () => {
      cancelled = true;
      if (timer) {
        clearTimeout(timer);
      }
    };
  }, [active, api, selectedRequestId, traceMode]);

  useKeyboard((key) => {
    if (!active) {
      return;
    }

    if (key.name === "1") {
      setTraceMode("off");
      return;
    }
    if (key.name === "2") {
      setTraceMode("prompt");
      return;
    }
    if (key.name === "3") {
      setTraceMode("debug");
      return;
    }
    if (key.name === "pageup") {
      detailRef.current?.scrollBy({ x: 0, y: -6 });
      return;
    }
    if (key.name === "pagedown") {
      detailRef.current?.scrollBy({ x: 0, y: 6 });
    }
  });

  return (
    <box
      width="100%"
      height="100%"
      flexDirection="column"
      gap={1}
      visible={active}
    >
      <box border title="Observability" padding={1} flexDirection="row" gap={2}>
        <box flexDirection="column" width="36%">
          <text>{summaryLines.join("\n")}</text>
        </box>
        <box flexDirection="column" flexGrow={1}>
          <text>
            Trace mode: {traceMode} | `1` off | `2` prompt | `3` debug | `PgUp/PgDn`
            detail scroll
          </text>
          <text>{statusLine}</text>
          <text>
            Top metrics:{" "}
            {topSamples
              .map((sample) =>
                `${sample.name}${Object.keys(sample.labels).length > 0 ? JSON.stringify(sample.labels) : ""}=${sample.value}`,
              )
              .join(" | ") || "-"}
          </text>
        </box>
      </box>

      <box flexDirection="row" flexGrow={1} gap={1}>
        <box
          border
          title="Request History"
          padding={1}
          width="38%"
          height="100%"
        >
          <select
            focused={active}
            width="100%"
            height="100%"
            options={requestOptions}
            selectedIndex={clampIndex(selectedIndex, requestOptions.length)}
            onChange={(index, option) => {
              setSelectedIndex(index);
              setSelectedRequestId(String(option?.value ?? ""));
            }}
            showScrollIndicator
            itemSpacing={1}
            wrapSelection
          />
        </box>

        <box border title="Execution Detail" padding={1} flexGrow={1} height="100%">
          <scrollbox
            ref={detailRef}
            width="100%"
            height="100%"
            scrollY
          >
            <text>{detailLines.join("\n")}</text>
          </scrollbox>
        </box>
      </box>
    </box>
  );
}
