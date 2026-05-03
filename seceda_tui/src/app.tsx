import { useKeyboard } from "@opentui/react";
import { useMemo, useState } from "react";

import { DEFAULT_BASE_URL, DEFAULT_CONFIG_PATH } from "./lib/paths.js";
import { SecedaApi } from "./lib/seceda_api.js";
import { CloudSetupScreen } from "./screens/cloud_setup.js";
import { ConfigurationScreen } from "./screens/configuration.js";
import { ObservabilityScreen } from "./screens/observability.js";

type ScreenId = "setup" | "observability" | "configuration";

export function App() {
  const [activeScreen, setActiveScreen] = useState<ScreenId>("setup");
  const api = useMemo(() => new SecedaApi(DEFAULT_BASE_URL), []);

  useKeyboard((key) => {
    if (key.name === "o") {
      setActiveScreen("observability");
      return;
    }
    if (key.name === "s") {
      setActiveScreen("setup");
      return;
    }
    if (key.name === "c") {
      setActiveScreen("configuration");
    }
  });

  return (
    <box
      width="100%"
      height="100%"
      flexDirection="column"
      padding={1}
      gap={1}
      backgroundColor="#10131a"
    >
      <box border padding={1} flexDirection="column">
        <text>Seceda Console</text>
        <text>
          Active screen: {activeScreen} | `s` setup | `o` observability | `c`
          configuration | base URL {api.baseUrl}
        </text>
        <text>Resolved config path: {DEFAULT_CONFIG_PATH}</text>
      </box>

      <box
        border
        flexGrow={1}
        position="relative"
        width="100%"
        height="100%"
        padding={1}
      >
        <box
          position="absolute"
          top={0}
          left={0}
          width="100%"
          height="100%"
          visible={activeScreen === "setup"}
        >
          <CloudSetupScreen api={api} active={activeScreen === "setup"} />
        </box>

        <box
          position="absolute"
          top={0}
          left={0}
          width="100%"
          height="100%"
          visible={activeScreen === "observability"}
        >
          <ObservabilityScreen api={api} active={activeScreen === "observability"} />
        </box>

        <box
          position="absolute"
          top={0}
          left={0}
          width="100%"
          height="100%"
          visible={activeScreen === "configuration"}
        >
          <ConfigurationScreen api={api} active={activeScreen === "configuration"} />
        </box>
      </box>
    </box>
  );
}
