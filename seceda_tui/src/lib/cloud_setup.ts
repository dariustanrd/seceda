export type CloudFallbackChoice =
  | "codex-subscription"
  | "modal"
  | "local-only";

export type CloudSetupStatus =
  | "not-started"
  | "choice-selected"
  | "authenticating"
  | "configured"
  | "local-only"
  | "failed";

export interface CloudSetupState {
  choice: CloudFallbackChoice;
  status: CloudSetupStatus;
  message: string;
}

export interface CloudSetupOption {
  id: CloudFallbackChoice;
  label: string;
  description: string;
}

export const CLOUD_SETUP_OPTIONS: CloudSetupOption[] = [
  {
    id: "codex-subscription",
    label: "Codex Subscription",
    description: "Use ChatGPT login for cloud fallback",
  },
  {
    id: "modal",
    label: "Modal",
    description: "Use the preserved Modal cloud runtime",
  },
  {
    id: "local-only",
    label: "Local only",
    description: "Continue without cloud fallback",
  },
];

export function initialCloudSetupState(): CloudSetupState {
  return {
    choice: "codex-subscription",
    status: "not-started",
    message: "Choose a cloud fallback endpoint.",
  };
}

export function selectCloudFallbackChoice(
  state: CloudSetupState,
  choice: CloudFallbackChoice,
): CloudSetupState {
  return {
    ...state,
    choice,
    status: "choice-selected",
    message:
      choice === "codex-subscription"
        ? "Codex Subscription selected. Start ChatGPT login to configure fallback."
        : choice === "modal"
          ? "Modal selected. Save this choice to use the Modal cloud runtime."
          : "Local-only selected. Save this choice to disable cloud fallback.",
  };
}

export function beginCloudSetup(state: CloudSetupState): CloudSetupState {
  if (state.choice === "local-only") {
    return {
      ...state,
      status: "local-only",
      message: "Local-only mode is ready. Cloud fallback remains unconfigured.",
    };
  }

  if (state.choice === "modal") {
    return {
      ...state,
      status: "configured",
      message: "Modal cloud fallback selected.",
    };
  }

  return {
    ...state,
    status: "authenticating",
    message: "Waiting for ChatGPT browser login to complete.",
  };
}

export function completeCodexLogin(
  state: CloudSetupState,
  ok: boolean,
  detail?: string,
): CloudSetupState {
  if (state.choice !== "codex-subscription") {
    return state;
  }

  return ok
    ? {
        ...state,
        status: "configured",
        message: "Codex Subscription cloud fallback is configured.",
      }
    : {
        ...state,
        status: "failed",
        message: `Codex Subscription login failed: ${detail ?? "unknown error"}`,
      };
}

export function skipCloudSetup(state: CloudSetupState): CloudSetupState {
  return {
    ...state,
    choice: "local-only",
    status: "local-only",
    message: "Local-only mode is ready. Cloud fallback remains unconfigured.",
  };
}
