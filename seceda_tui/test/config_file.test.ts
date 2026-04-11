import { describe, expect, test } from "bun:test";

import type { CatalogField } from "../src/lib/config_catalog.js";
import {
  applyConfigEdits,
  derivePlaceholderContext,
  getValueAtPath,
  readConfigFieldState,
  resolveConfigPath,
  type LoadedConfigFile,
} from "../src/lib/config_file.js";

function makeField(overrides: Partial<CatalogField>): CatalogField {
  return {
    key: overrides.key ?? "test.field",
    group: overrides.group ?? "Tests",
    label: overrides.label ?? "Test field",
    description: overrides.description ?? "",
    type: overrides.type ?? "string",
    configPath: overrides.configPath ?? "daemon.host",
    tuiFieldId: overrides.tuiFieldId ?? "test.field",
    defaultValue: overrides.defaultValue ?? "",
    enumValues: overrides.enumValues ?? [],
    sensitive: overrides.sensitive ?? false,
    experimental: overrides.experimental ?? false,
    advanced: overrides.advanced ?? false,
    hotReloadable: overrides.hotReloadable ?? false,
    requiresRestart: overrides.requiresRestart ?? false,
    envVar: overrides.envVar ?? "",
    minValue: overrides.minValue ?? "",
    maxValue: overrides.maxValue ?? "",
    cliFlag: overrides.cliFlag ?? "",
    cliValueName: overrides.cliValueName ?? "",
  };
}

function makeLoadedConfig(): LoadedConfigFile {
  const document = {
    local: {
      active_engine: "llama-default",
      engines: {
        "llama-default": {
          model_path: "demo.gguf",
        },
      },
    },
    remote: {
      default_backend: "modal-default",
      backends: {
        "modal-default": {
          api_key: "super-secret",
        },
      },
    },
    router: {
      cloud_keywords: ["web", "research"],
    },
  } satisfies Record<string, unknown>;

  return {
    path: "/tmp/seceda.toml",
    document,
    context: derivePlaceholderContext(document),
  };
}

describe("config file helpers", () => {
  test("resolveConfigPath uses active engine and default backend placeholders", () => {
    const loaded = makeLoadedConfig();

    expect(
      resolveConfigPath("local.engines.<active>.model_path", loaded.context),
    ).toEqual(["local", "engines", "llama-default", "model_path"]);
    expect(
      resolveConfigPath("remote.backends.<default>.api_key", loaded.context),
    ).toEqual(["remote", "backends", "modal-default", "api_key"]);
  });

  test("readConfigFieldState masks sensitive values and applies defaults", () => {
    const loaded = makeLoadedConfig();
    const apiKeyField = makeField({
      key: "remote.default.api_key",
      label: "API key",
      configPath: "remote.backends.<default>.api_key",
      tuiFieldId: "remote.default.api_key",
      sensitive: true,
    });
    const maxTokensField = makeField({
      key: "generation.max_completion_tokens",
      label: "Max tokens",
      type: "integer",
      configPath: "generation.max_completion_tokens",
      tuiFieldId: "generation.max_completion_tokens",
      defaultValue: "128",
    });

    const apiKeyState = readConfigFieldState(loaded, apiKeyField);
    const maxTokensState = readConfigFieldState(loaded, maxTokensField);

    expect(apiKeyState.maskedValue).not.toBe("super-secret");
    expect(maxTokensState.displayValue).toBe("128");
    expect(maxTokensState.source).toBe("default");
  });

  test("applyConfigEdits writes scalar and csv values back to the resolved path", () => {
    const loaded = makeLoadedConfig();
    const modelPathField = makeField({
      key: "local.active.model_path",
      label: "Model path",
      configPath: "local.engines.<active>.model_path",
      tuiFieldId: "local.active.model_path",
    });
    const cloudKeywordsField = makeField({
      key: "router.cloud_keywords",
      label: "Cloud keywords",
      type: "csv",
      configPath: "router.cloud_keywords",
      tuiFieldId: "router.cloud_keywords",
    });

    const applied = applyConfigEdits(loaded, [
      { field: modelPathField, input: "next.gguf" },
      { field: cloudKeywordsField, input: "web, research, freshness" },
    ]);

    expect(
      getValueAtPath(applied.document, ["local", "engines", "llama-default", "model_path"]),
    ).toBe("next.gguf");
    expect(getValueAtPath(applied.document, ["router", "cloud_keywords"])).toEqual([
      "web",
      "research",
      "freshness",
    ]);
  });
});
