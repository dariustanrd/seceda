import { readFile, rename, writeFile } from "node:fs/promises";

import { parse, stringify } from "smol-toml";

import type { CatalogField } from "./config_catalog.js";
import { DEFAULT_CONFIG_PATH } from "./paths.js";

export interface PlaceholderContext {
  activeLocalEngine: string;
  defaultRemoteBackend: string;
}

export interface LoadedConfigFile {
  path: string;
  document: Record<string, unknown>;
  context: PlaceholderContext;
}

export interface ConfigFieldState {
  field: CatalogField;
  resolvedPath: string | null;
  value: unknown;
  displayValue: string;
  maskedValue: string;
  source: "config" | "default" | "missing";
}

export interface ConfigFieldEdit {
  field: CatalogField;
  input: string;
}

export interface AppliedConfigEdits {
  document: Record<string, unknown>;
  changedFields: CatalogField[];
}

export interface SaveConfigResult {
  path: string;
  changedFieldIds: string[];
  restartRequired: boolean;
  hotReloadableOnly: boolean;
  rendered: string;
}

function asRecord(value: unknown): Record<string, unknown> | null {
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    return null;
  }
  return value as Record<string, unknown>;
}

function getStringValue(value: unknown): string {
  return typeof value === "string" ? value : "";
}

function getObjectKeys(value: unknown): string[] {
  const record = asRecord(value);
  return record ? Object.keys(record) : [];
}

function cloneDocument(document: Record<string, unknown>): Record<string, unknown> {
  return structuredClone(document) as Record<string, unknown>;
}

export async function loadConfigFile(
  path = DEFAULT_CONFIG_PATH,
): Promise<LoadedConfigFile> {
  const raw = await readFile(path, "utf8");
  const document = parse(raw) as Record<string, unknown>;

  return {
    path,
    document,
    context: derivePlaceholderContext(document),
  };
}

export function derivePlaceholderContext(
  document: Record<string, unknown>,
): PlaceholderContext {
  const local = asRecord(document.local);
  const remote = asRecord(document.remote);

  const localEngines = getObjectKeys(local?.engines);
  const remoteBackends = getObjectKeys(remote?.backends);

  return {
    activeLocalEngine:
      getStringValue(local?.active_engine) || localEngines[0] || "llama-default",
    defaultRemoteBackend:
      getStringValue(remote?.default_backend) || remoteBackends[0] || "modal-default",
  };
}

export function resolveConfigPath(
  configPath: string,
  context: PlaceholderContext,
): string[] | null {
  const segments = configPath.split(".");
  const resolved: string[] = [];

  for (const segment of segments) {
    if (!segment || segment.includes("[]") || segment.includes("<name>")) {
      return null;
    }
    if (segment === "<active>") {
      resolved.push(context.activeLocalEngine);
      continue;
    }
    if (segment === "<default>") {
      resolved.push(context.defaultRemoteBackend);
      continue;
    }
    resolved.push(segment);
  }

  return resolved;
}

export function getValueAtPath(
  document: Record<string, unknown>,
  path: string[],
): unknown {
  let current: unknown = document;
  for (const segment of path) {
    const record = asRecord(current);
    if (!record || !(segment in record)) {
      return undefined;
    }
    current = record[segment];
  }
  return current;
}

function ensureRecord(
  document: Record<string, unknown>,
  path: string[],
): Record<string, unknown> {
  let current = document;
  for (const segment of path) {
    const next = asRecord(current[segment]);
    if (next) {
      current = next;
      continue;
    }

    const replacement: Record<string, unknown> = {};
    current[segment] = replacement;
    current = replacement;
  }
  return current;
}

export function setValueAtPath(
  document: Record<string, unknown>,
  path: string[],
  value: unknown,
): void {
  if (path.length === 0) {
    return;
  }

  const parent = ensureRecord(document, path.slice(0, -1));
  parent[path[path.length - 1]] = value;
}

function parseCatalogDefault(field: CatalogField): unknown {
  if (!field.defaultValue) {
    return undefined;
  }
  return coerceFieldInput(field, field.defaultValue);
}

export function coerceFieldInput(field: CatalogField, input: string): unknown {
  const trimmed = input.trim();

  switch (field.type) {
    case "integer": {
      const value = Number.parseInt(trimmed, 10);
      if (!Number.isFinite(value)) {
        throw new Error(`Expected integer for ${field.label}`);
      }
      return value;
    }
    case "float": {
      const value = Number.parseFloat(trimmed);
      if (!Number.isFinite(value)) {
        throw new Error(`Expected number for ${field.label}`);
      }
      return value;
    }
    case "boolean": {
      if (trimmed === "true") {
        return true;
      }
      if (trimmed === "false") {
        return false;
      }
      throw new Error(`Expected true or false for ${field.label}`);
    }
    case "csv":
      return trimmed
        ? trimmed
            .split(",")
            .map((piece) => piece.trim())
            .filter(Boolean)
        : [];
    case "string":
      return input;
    default:
      throw new Error(`Unsupported field type ${field.type}`);
  }
}

export function formatFieldValue(field: CatalogField, value: unknown): string {
  if (value === undefined || value === null) {
    return "";
  }
  if (field.type === "csv") {
    if (Array.isArray(value)) {
      return value.map((entry) => String(entry)).join(", ");
    }
    return String(value);
  }
  if (field.type === "boolean") {
    return value ? "true" : "false";
  }
  return String(value);
}

export function maskSensitiveValue(value: string): string {
  if (!value) {
    return "";
  }
  return "*".repeat(Math.max(4, Math.min(value.length, 16)));
}

export function readConfigFieldState(
  loaded: LoadedConfigFile,
  field: CatalogField,
): ConfigFieldState {
  const resolvedPath = resolveConfigPath(field.configPath, loaded.context);
  const rawValue = resolvedPath ? getValueAtPath(loaded.document, resolvedPath) : undefined;
  const defaultValue = parseCatalogDefault(field);
  const value =
    rawValue !== undefined ? rawValue : defaultValue !== undefined ? defaultValue : undefined;
  const source =
    rawValue !== undefined
      ? "config"
      : defaultValue !== undefined
        ? "default"
        : "missing";
  const displayValue = formatFieldValue(field, value);

  return {
    field,
    resolvedPath: resolvedPath ? resolvedPath.join(".") : null,
    value,
    displayValue,
    maskedValue: field.sensitive ? maskSensitiveValue(displayValue) : displayValue,
    source,
  };
}

export function buildConfigFieldStateMap(
  loaded: LoadedConfigFile,
  fields: CatalogField[],
): Map<string, ConfigFieldState> {
  return new Map(
    fields.map((field) => [field.tuiFieldId, readConfigFieldState(loaded, field)]),
  );
}

export function applyConfigEdits(
  loaded: LoadedConfigFile,
  edits: ConfigFieldEdit[],
): AppliedConfigEdits {
  const nextDocument = cloneDocument(loaded.document);
  const changedFields: CatalogField[] = [];

  for (const edit of edits) {
    const resolvedPath = resolveConfigPath(edit.field.configPath, loaded.context);
    if (!resolvedPath) {
      continue;
    }

    const nextValue = coerceFieldInput(edit.field, edit.input);
    const currentValue = getValueAtPath(nextDocument, resolvedPath);
    if (JSON.stringify(currentValue) === JSON.stringify(nextValue)) {
      continue;
    }

    setValueAtPath(nextDocument, resolvedPath, nextValue);
    changedFields.push(edit.field);
  }

  return {
    document: nextDocument,
    changedFields,
  };
}

export async function saveConfigEdits(
  loaded: LoadedConfigFile,
  edits: ConfigFieldEdit[],
): Promise<SaveConfigResult> {
  const applied = applyConfigEdits(loaded, edits);
  const rendered = stringify(applied.document);
  const tempPath = `${loaded.path}.tmp`;

  await writeFile(tempPath, rendered, "utf8");
  await rename(tempPath, loaded.path);

  return {
    path: loaded.path,
    changedFieldIds: applied.changedFields.map((field) => field.tuiFieldId),
    restartRequired: applied.changedFields.some((field) => field.requiresRestart),
    hotReloadableOnly:
      applied.changedFields.length > 0 &&
      applied.changedFields.every((field) => field.hotReloadable && !field.requiresRestart),
    rendered,
  };
}
