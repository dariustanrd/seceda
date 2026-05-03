import type { SelectOption } from "@opentui/core";
import { useKeyboard } from "@opentui/react";
import { useMemo, useState } from "react";

import {
  beginCloudSetup,
  CLOUD_SETUP_OPTIONS,
  completeCodexLogin,
  initialCloudSetupState,
  selectCloudFallbackChoice,
  skipCloudSetup,
  type CloudFallbackChoice,
} from "../lib/cloud_setup.js";
import type { SecedaApi } from "../lib/seceda_api.js";

interface CloudSetupScreenProps {
  api: SecedaApi;
  active: boolean;
}

function clampIndex(index: number, length: number): number {
  if (length <= 0) {
    return 0;
  }
  return Math.max(0, Math.min(index, length - 1));
}

export function CloudSetupScreen({ api, active }: CloudSetupScreenProps) {
  const [selectedIndex, setSelectedIndex] = useState(0);
  const [state, setState] = useState(initialCloudSetupState());

  const options = useMemo<SelectOption[]>(
    () =>
      CLOUD_SETUP_OPTIONS.map((option) => ({
        name: option.label,
        description: option.description,
        value: option.id,
      })),
    [],
  );

  useKeyboard((key) => {
    if (!active) {
      return;
    }

    if (key.name === "escape") {
      setState((current) => skipCloudSetup(current));
      return;
    }

    if (key.name !== "return") {
      return;
    }

    setState((current) => {
      const next = beginCloudSetup(current);
      if (next.choice !== "codex-subscription") {
        return next;
      }

      void api
        .startCodexSubscriptionLogin()
        .then((result) => {
          setState((latest) =>
            completeCodexLogin(
              latest,
              result.ok,
              result.message ?? result.auth_url ?? "login did not complete",
            ),
          );
        })
        .catch((error) => {
          setState((latest) => completeCodexLogin(latest, false, String(error)));
        });
      return next;
    });
  });

  return (
    <box
      width="100%"
      height="100%"
      flexDirection="column"
      gap={1}
      visible={active}
    >
      <box border title="Cloud Setup" padding={1} flexDirection="column">
        <text>
          Status: {state.status} | `Enter` apply/start login | `Esc` local-only
        </text>
        <text>{state.message}</text>
        <text>Selected endpoint: {state.choice}</text>
      </box>

      <box flexDirection="row" flexGrow={1} gap={1}>
        <box border title="Fallback Endpoint" padding={1} width="38%" height="100%">
          <select
            focused={active}
            width="100%"
            height="100%"
            options={options}
            selectedIndex={clampIndex(selectedIndex, options.length)}
            onChange={(index, option) => {
              setSelectedIndex(index);
              setState((current) =>
                selectCloudFallbackChoice(
                  current,
                  String(option?.value ?? "codex-subscription") as CloudFallbackChoice,
                ),
              );
            }}
            showScrollIndicator
            itemSpacing={1}
            wrapSelection
          />
        </box>

        <box border title="Setup State" padding={1} flexGrow={1} height="100%">
          <text>
            {state.choice === "codex-subscription"
              ? "ChatGPT browser login stores a Seceda-owned credential under ~/.seceda."
              : state.choice === "modal"
                ? "Modal remains a separate cloud runtime option."
                : "Local routing stays available without cloud fallback."}
          </text>
        </box>
      </box>
    </box>
  );
}
