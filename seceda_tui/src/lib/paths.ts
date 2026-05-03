import { dirname, isAbsolute, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const LIB_DIR = dirname(fileURLToPath(import.meta.url));

export const TUI_ROOT = resolve(LIB_DIR, "../..");
export const REPO_ROOT = resolve(TUI_ROOT, "..");

export function resolveRepoPath(value: string): string {
  return isAbsolute(value) ? value : resolve(REPO_ROOT, value);
}

export function normalizeBaseUrl(value: string): string {
  return value.replace(/\/+$/, "") || "http://127.0.0.1:8080";
}

export const DEFAULT_BASE_URL = normalizeBaseUrl(
  process.env.SECEDA_TUI_BASE_URL ?? "http://127.0.0.1:8080",
);

export const DEFAULT_CATALOG_PATH = resolveRepoPath(
  process.env.SECEDA_TUI_CATALOG_PATH ?? "seceda_edge/config/config_catalog.toml",
);

export const DEFAULT_CONFIG_PATH = resolveRepoPath(
  process.env.SECEDA_TUI_CONFIG_PATH ?? "seceda_edge/config/seceda.toml",
);
