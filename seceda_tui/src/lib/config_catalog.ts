import { readFile } from "node:fs/promises";

import { parse } from "smol-toml";

import { DEFAULT_CATALOG_PATH } from "./paths.js";

export type CatalogFieldType =
  | "string"
  | "integer"
  | "float"
  | "boolean"
  | "csv"
  | "spec";

interface RawCatalogField {
  key?: string;
  group?: string;
  label?: string;
  description?: string;
  type?: string;
  config_path?: string;
  tui_field_id?: string;
  default?: string;
  enum_values?: string[];
  sensitive?: boolean;
  experimental?: boolean;
  advanced?: boolean;
  hot_reloadable?: boolean;
  requires_restart?: boolean;
  env_var?: string;
  min?: string;
  max?: string;
  cli_flag?: string;
  cli_value_name?: string;
}

interface RawCatalogDocument {
  fields?: RawCatalogField[];
}

export interface CatalogField {
  key: string;
  group: string;
  label: string;
  description: string;
  type: CatalogFieldType;
  configPath: string;
  tuiFieldId: string;
  defaultValue: string;
  enumValues: string[];
  sensitive: boolean;
  experimental: boolean;
  advanced: boolean;
  hotReloadable: boolean;
  requiresRestart: boolean;
  envVar: string;
  minValue: string;
  maxValue: string;
  cliFlag: string;
  cliValueName: string;
}

export interface CatalogGroup {
  name: string;
  fields: CatalogField[];
}

function normalizeCatalogField(field: RawCatalogField): CatalogField | null {
  if (!field.key || !field.group || !field.label || !field.type) {
    return null;
  }
  if (!field.config_path || !field.tui_field_id) {
    return null;
  }
  if (field.type === "spec") {
    return null;
  }

  return {
    key: field.key,
    group: field.group,
    label: field.label,
    description: field.description ?? "",
    type: field.type as CatalogFieldType,
    configPath: field.config_path,
    tuiFieldId: field.tui_field_id,
    defaultValue: field.default ?? "",
    enumValues: [...(field.enum_values ?? [])],
    sensitive: field.sensitive ?? false,
    experimental: field.experimental ?? false,
    advanced: field.advanced ?? false,
    hotReloadable: field.hot_reloadable ?? false,
    requiresRestart: field.requires_restart ?? false,
    envVar: field.env_var ?? "",
    minValue: field.min ?? "",
    maxValue: field.max ?? "",
    cliFlag: field.cli_flag ?? "",
    cliValueName: field.cli_value_name ?? "",
  };
}

export async function loadConfigCatalog(
  path = DEFAULT_CATALOG_PATH,
): Promise<CatalogField[]> {
  const raw = await readFile(path, "utf8");
  const parsed = parse(raw) as RawCatalogDocument;

  return (parsed.fields ?? [])
    .map(normalizeCatalogField)
    .filter((field): field is CatalogField => field !== null);
}

export function groupCatalogFields(fields: CatalogField[]): CatalogGroup[] {
  const groups = new Map<string, CatalogGroup>();

  for (const field of fields) {
    const existing = groups.get(field.group);
    if (existing) {
      existing.fields.push(field);
      continue;
    }

    groups.set(field.group, {
      name: field.group,
      fields: [field],
    });
  }

  return [...groups.values()];
}

export function fieldEditorKind(
  field: CatalogField,
): "select" | "boolean" | "input" {
  if (field.type === "boolean") {
    return "boolean";
  }
  if (field.enumValues.length > 0) {
    return "select";
  }
  return "input";
}
