import { useKeyboard } from "@opentui/react";
import { useMemo, useState } from "react";

import { DEFAULT_BASE_URL, DEFAULT_CONFIG_PATH } from "./lib/paths.js";
import { SecedaApi } from "./lib/seceda_api.js";
import { ConfigurationScreen } from "./screens/configuration.js";
import { ObservabilityScreen } from "./screens/observability.js";

type ScreenId = "observability" | "configuration";

export function App() {
  const [activeScreen, setActiveScreen] = useState<ScreenId>("observability");
  const api = useMemo(() => new SecedaApi(DEFAULT_BASE_URL), []);

  useKeyboard((key) => {
    if (key.name === "f1") {
      setActiveScreen("observability");
      return;
    }
    if (key.name === "f2") {
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
          Active screen: {activeScreen} | `F1` observability | `F2` configuration | base
          URL {api.baseUrl}
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
