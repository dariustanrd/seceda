import type { SelectOption } from "@opentui/core";
import { useKeyboard } from "@opentui/react";
import { useCallback, useEffect, useMemo, useState } from "react";

import {
  fieldEditorKind,
  groupCatalogFields,
  loadConfigCatalog,
  type CatalogGroup,
} from "../lib/config_catalog.js";
import {
  buildConfigFieldStateMap,
  loadConfigFile,
  maskSensitiveValue,
  saveConfigEdits,
  type ConfigFieldState,
  type LoadedConfigFile,
} from "../lib/config_file.js";
import type { SecedaApi } from "../lib/seceda_api.js";

interface ConfigurationScreenProps {
  api: SecedaApi;
  active: boolean;
}

type FocusTarget = "groups" | "fields" | "editor";

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

function buildFieldDescription(
  fieldState: ConfigFieldState | null,
  editorValue: string,
  hasDraft: boolean,
): string[] {
  if (!fieldState) {
    return ["Select a config field to inspect or edit."];
  }

  const field = fieldState.field;
  const lines = [
    `label: ${field.label}`,
    `group: ${field.group}`,
    `type: ${field.type}`,
    `config_path: ${field.configPath}`,
    `resolved_path: ${fieldState.resolvedPath ?? "-"}`,
    `source: ${fieldState.source}`,
    `current: ${fieldState.maskedValue || "-"}`,
    `editing: ${field.sensitive ? maskSensitiveValue(editorValue) || "(empty)" : editorValue || "(empty)"}`,
    `dirty: ${hasDraft ? "yes" : "no"}`,
    `description: ${field.description || "-"}`,
  ];

  if (field.enumValues.length > 0) {
    lines.push(`enum: ${field.enumValues.join(", ")}`);
  }
  if (field.minValue || field.maxValue) {
    lines.push(`range: ${field.minValue || "-"} .. ${field.maxValue || "-"}`);
  }
  if (field.envVar) {
    lines.push(`env_var: ${field.envVar}`);
  }

  lines.push(
    `restart_guidance: ${
      field.requiresRestart
        ? "daemon restart required"
        : field.hotReloadable
          ? "hot reloadable"
          : "save config, then use /admin/model where appropriate"
    }`,
  );

  return lines;
}

export function ConfigurationScreen({
  api,
  active,
}: ConfigurationScreenProps) {
  const [groups, setGroups] = useState<CatalogGroup[]>([]);
  const [loadedConfig, setLoadedConfig] = useState<LoadedConfigFile | null>(null);
  const [fieldStates, setFieldStates] = useState<Map<string, ConfigFieldState>>(new Map());
  const [groupIndex, setGroupIndex] = useState(0);
  const [fieldIndex, setFieldIndex] = useState(0);
  const [focusTarget, setFocusTarget] = useState<FocusTarget>("fields");
  const [drafts, setDrafts] = useState<Record<string, string>>({});
  const [editorValue, setEditorValue] = useState("");
  const [editorNonce, setEditorNonce] = useState(0);
  const [loadedVersion, setLoadedVersion] = useState(0);
  const [statusLine, setStatusLine] = useState("Loading config catalog...");

  const selectedGroup = groups[clampIndex(groupIndex, groups.length)] ?? null;
  const selectedField =
    selectedGroup?.fields[clampIndex(fieldIndex, selectedGroup.fields.length)] ?? null;
  const selectedFieldState = selectedField
    ? fieldStates.get(selectedField.tuiFieldId) ?? null
    : null;
  const selectedEditorKind = selectedField
    ? fieldEditorKind(selectedField)
    : "input";

  const allFields = useMemo(() => groups.flatMap((group) => group.fields), [groups]);

  const pendingEdits = useMemo(() => {
    return allFields.flatMap((field) => {
      const draft = drafts[field.tuiFieldId];
      const fieldState = fieldStates.get(field.tuiFieldId);
      if (draft === undefined || !fieldState) {
        return [];
      }
      if (field.sensitive && draft === "") {
        return [];
      }
      if (!field.sensitive && draft === fieldState.displayValue) {
        return [];
      }
      return [{ field, input: draft }];
    });
  }, [allFields, drafts, fieldStates]);

  const editorOptions = useMemo<SelectOption[]>(() => {
    if (!selectedField) {
      return [];
    }
    if (selectedEditorKind === "boolean") {
      return [
        { name: "true", description: "Enable this option", value: "true" },
        { name: "false", description: "Disable this option", value: "false" },
      ];
    }
    if (selectedEditorKind === "select") {
      return selectedField.enumValues.map((value) => ({
        name: value,
        description: value,
        value,
      }));
    }
    return [];
  }, [selectedEditorKind, selectedField]);

  const fieldDescription = useMemo(
    () =>
      buildFieldDescription(
        selectedFieldState,
        editorValue,
        Boolean(selectedField && drafts[selectedField.tuiFieldId] !== undefined),
      ),
    [drafts, editorValue, selectedField, selectedFieldState],
  );

  const reloadFromDisk = useCallback(async () => {
    try {
      const [catalogFields, nextConfig] = await Promise.all([
        loadConfigCatalog(),
        loadConfigFile(loadedConfig?.path),
      ]);
      const nextGroups = groupCatalogFields(catalogFields);
      setGroups(nextGroups);
      setLoadedConfig(nextConfig);
      setFieldStates(buildConfigFieldStateMap(nextConfig, catalogFields));
      setLoadedVersion((value) => value + 1);
      setStatusLine(
        `Loaded ${catalogFields.length} editable fields from ${nextConfig.path}.`,
      );
    } catch (error) {
      setStatusLine(`Config reload failed: ${String(error)}`);
    }
  }, [loadedConfig?.path]);

  useEffect(() => {
    void reloadFromDisk();
  }, [reloadFromDisk]);

  useEffect(() => {
    setGroupIndex((current) => clampIndex(current, groups.length));
  }, [groups.length]);

  useEffect(() => {
    if (!selectedGroup) {
      setFieldIndex(0);
      return;
    }
    setFieldIndex((current) => clampIndex(current, selectedGroup.fields.length));
  }, [selectedGroup]);

  useEffect(() => {
    if (!selectedField) {
      setEditorValue("");
      return;
    }

    const nextDraft = drafts[selectedField.tuiFieldId];
    const nextFieldState = fieldStates.get(selectedField.tuiFieldId);
    const nextValue =
      nextDraft !== undefined
        ? nextDraft
        : selectedField.sensitive
          ? ""
          : nextFieldState?.displayValue ?? "";

    setEditorValue(nextValue);
    setEditorNonce((value) => value + 1);
  }, [loadedVersion, selectedField?.tuiFieldId]);

  const updateDraft = useCallback(
    (nextValue: string) => {
      setEditorValue(nextValue);

      if (!selectedField || !selectedFieldState) {
        return;
      }

      setDrafts((current) => {
        const next = { ...current };
        if (
          (selectedField.sensitive && nextValue === "") ||
          (!selectedField.sensitive && nextValue === selectedFieldState.displayValue)
        ) {
          delete next[selectedField.tuiFieldId];
          return next;
        }

        next[selectedField.tuiFieldId] = nextValue;
        return next;
      });
    },
    [selectedField, selectedFieldState],
  );

  const saveChanges = useCallback(async () => {
    if (!loadedConfig) {
      setStatusLine("Config file is not loaded yet.");
      return;
    }
    if (pendingEdits.length === 0) {
      setStatusLine("No config changes to save.");
      return;
    }

    try {
      const result = await saveConfigEdits(loadedConfig, pendingEdits);
      setDrafts({});
      await reloadFromDisk();
      setStatusLine(
        `Saved ${result.changedFieldIds.length} field(s). ${
          result.restartRequired
            ? "Restart the daemon to apply all changes."
            : result.hotReloadableOnly
              ? "All saved fields are hot reloadable."
              : "Consider reloading the local model if runtime settings changed."
        }`,
      );
    } catch (error) {
      setStatusLine(`Save failed: ${String(error)}`);
    }
  }, [loadedConfig, pendingEdits, reloadFromDisk]);

  const reloadModel = useCallback(async () => {
    const modelPathState = fieldStates.get("local.active.model_path");
    const warmupPromptState = fieldStates.get("daemon.warmup_prompt");
    const modelPath =
      drafts["local.active.model_path"] ?? modelPathState?.displayValue ?? "";
    const warmupPrompt =
      drafts["daemon.warmup_prompt"] ?? warmupPromptState?.displayValue ?? "";

    if (!modelPath) {
      setStatusLine("Set `local.active.model_path` before using /admin/model.");
      return;
    }

    try {
      const result = await api.reloadModel(modelPath, warmupPrompt);
      setStatusLine(
        result.ok
          ? `Model reload ok. Active path: ${result.active_model_path ?? modelPath}`
          : `Model reload failed: ${result.error ?? "unknown error"}`,
      );
    } catch (error) {
      setStatusLine(`Model reload request failed: ${String(error)}`);
    }
  }, [api, drafts, fieldStates]);

  useKeyboard((key) => {
    if (!active) {
      return;
    }

    if (key.name === "tab") {
      setFocusTarget((current) =>
        current === "groups" ? "fields" : current === "fields" ? "editor" : "groups",
      );
      return;
    }

    if (focusTarget === "editor") {
      return;
    }

    if (key.name === "s") {
      void saveChanges();
      return;
    }
    if (key.name === "r") {
      void reloadFromDisk();
      return;
    }
    if (key.name === "l") {
      void reloadModel();
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
      <box border title="Configuration" padding={1} flexDirection="column">
        <text>
          Pending edits: {pendingEdits.length} | Focus: {focusTarget} | `Tab` cycle focus
          | `s` save | `r` reload file | `l` reload model
        </text>
        <text>{statusLine}</text>
        <text>Config path: {loadedConfig?.path ?? "-"}</text>
      </box>

      <box flexDirection="row" flexGrow={1} gap={1}>
        <box border title="Groups" padding={1} width="24%" height="100%">
          <select
            focused={active && focusTarget === "groups"}
            width="100%"
            height="100%"
            options={groups.map((group) => ({
              name: group.name,
              description: `${group.fields.length} fields`,
              value: group.name,
            }))}
            selectedIndex={clampIndex(groupIndex, groups.length)}
            onChange={(index) => {
              setGroupIndex(index);
              setFieldIndex(0);
            }}
            showScrollIndicator
            wrapSelection
          />
        </box>

        <box border title="Fields" padding={1} width="34%" height="100%">
          <select
            focused={active && focusTarget === "fields"}
            width="100%"
            height="100%"
            options={(selectedGroup?.fields ?? []).map((field) => {
              const state = fieldStates.get(field.tuiFieldId);
              const suffix = drafts[field.tuiFieldId] !== undefined ? " *" : "";
              return {
                name: `${field.label}${suffix}`,
                description:
                  state?.maskedValue || field.description || field.configPath || "-",
                value: field.tuiFieldId,
              };
            })}
            selectedIndex={clampIndex(fieldIndex, selectedGroup?.fields.length ?? 0)}
            onChange={(index) => {
              setFieldIndex(index);
            }}
            showScrollIndicator
            itemSpacing={1}
            wrapSelection
          />
        </box>

        <box border title="Editor" padding={1} flexGrow={1} height="100%" gap={1}>
          <box title="Current field" border padding={1}>
            <text>{fieldDescription.join("\n")}</text>
          </box>

          <box title="Input" border padding={1} height={selectedEditorKind === "input" ? 3 : 8}>
            {selectedField ? (
              selectedEditorKind === "input" ? (
                <input
                  key={`${selectedField.tuiFieldId}:${editorNonce}`}
                  focused={active && focusTarget === "editor"}
                  width="100%"
                  placeholder={
                    selectedField.sensitive
                      ? "Enter new value; leave blank to keep current secret"
                      : selectedField.label
                  }
                  value={editorValue}
                  onInput={updateDraft}
                />
              ) : (
                <select
                  key={`${selectedField.tuiFieldId}:${editorNonce}`}
                  focused={active && focusTarget === "editor"}
                  width="100%"
                  height={6}
                  options={editorOptions}
                  selectedIndex={Math.max(
                    0,
                    editorOptions.findIndex(
                      (option) => String(option.value ?? option.name) === editorValue,
                    ),
                  )}
                  onChange={(_, option) => updateDraft(String(option?.value ?? ""))}
                  onSelect={(_, option) => updateDraft(String(option?.value ?? ""))}
                  wrapSelection
                />
              )
            ) : (
              <text>Select a field to edit.</text>
            )}
          </box>

          <box title="Raw value preview" border padding={1} flexGrow={1}>
            <text>
              {selectedFieldState
                ? prettyValue(selectedFieldState.value) || "(empty)"
                : "No field selected."}
            </text>
          </box>
        </box>
      </box>
    </box>
  );
}
