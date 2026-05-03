# Seceda TUI

OpenTUI-based operator console for Seceda.

## Run

```bash
bun install
bun test
bun run check
bun run src/index.tsx
```

Environment overrides:

```bash
SECEDA_TUI_BASE_URL=http://127.0.0.1:8080
SECEDA_TUI_CONFIG_PATH=seceda_edge/config/seceda.toml
SECEDA_TUI_CATALOG_PATH=seceda_edge/config/config_catalog.toml
```

## Screens

- `F1`: observability
- `F2`: configuration

Observability shortcuts:

- `1`: disable prompt/body trace fetch
- `2`: show prompt-oriented trace view
- `3`: show debug trace view
- `PgUp` / `PgDn`: scroll the detail pane

Configuration shortcuts:

- `Tab`: cycle focus across groups, fields, and editor
- `s`: save pending edits
- `r`: reload the config file from disk
- `l`: call `/admin/model` using the current `local.active.model_path`

## Manual Verification

1. Start the edge daemon on localhost.
2. Run the TUI and confirm `GET /info`, `GET /v1/models`, `GET /metrics`, and `GET /metrics/events` populate the observability screen.
3. Send a non-streaming `POST /v1/chat/completions` request and confirm its public `id` resolves into the request list and detail pane.
4. Send a streaming `POST /v1/chat/completions` request and confirm the same `request_id` joins summary metadata with prompt/body trace detail.
5. Switch between trace modes `1`, `2`, and `3` to confirm prompt/debug panes only load when enabled.
6. Edit a config field, save it, and verify the config file changes plus restart guidance.
7. Change `local.active.model_path`, press `l`, and verify `/admin/model` reload feedback.

## Hardening Notes

- Keep `SECEDA_TUI_BASE_URL` on loopback unless the daemon is behind authentication and TLS.
- Prompt/body trace views can expose prompt text, outputs, and tool arguments; treat them as operator-only surfaces.
- `remote.default.api_key` is editable, but `SECEDA_CLOUD_API_KEY` is still the preferred production secret path.
- Saving config rewrites the TOML file through a serializer; comments and some original formatting are not preserved.
