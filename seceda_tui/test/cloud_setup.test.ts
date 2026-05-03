import { describe, expect, test } from "bun:test";

import {
  beginCloudSetup,
  completeCodexLogin,
  initialCloudSetupState,
  selectCloudFallbackChoice,
  skipCloudSetup,
} from "../src/lib/cloud_setup.js";

describe("cloud fallback setup state", () => {
  test("defaults to Codex Subscription as the first guided option", () => {
    const state = initialCloudSetupState();

    expect(state.choice).toBe("codex-subscription");
    expect(state.status).toBe("not-started");
  });

  test("skipping setup leaves Seceda in local-only mode", () => {
    const state = skipCloudSetup(initialCloudSetupState());

    expect(state.choice).toBe("local-only");
    expect(state.status).toBe("local-only");
    expect(state.message).toContain("Cloud fallback remains unconfigured");
  });

  test("Codex Subscription choice enters browser login flow", () => {
    const selected = selectCloudFallbackChoice(
      initialCloudSetupState(),
      "codex-subscription",
    );
    const authenticating = beginCloudSetup(selected);
    const configured = completeCodexLogin(authenticating, true);

    expect(authenticating.status).toBe("authenticating");
    expect(configured.status).toBe("configured");
  });

  test("Codex Subscription failed login is visible to the setup screen", () => {
    const authenticating = beginCloudSetup(initialCloudSetupState());
    const failed = completeCodexLogin(authenticating, false, "access denied");

    expect(failed.status).toBe("failed");
    expect(failed.message).toContain("access denied");
  });

  test("Modal selection configures a separate cloud runtime option", () => {
    const selected = selectCloudFallbackChoice(initialCloudSetupState(), "modal");
    const configured = beginCloudSetup(selected);

    expect(configured.choice).toBe("modal");
    expect(configured.status).toBe("configured");
  });
});
