# Package Layout

Seceda is now scaffolded as a Rust-first workspace while keeping the existing
cloud and TUI packages as first-class project areas.

## Rust Workspace

The root `Cargo.toml` defines these workspace members:

- `crates/seceda-core`: shared request, model, and routing contracts.
- `crates/seceda-server`: localhost OpenAI-compatible server boundary.
- `crates/seceda-llama`: `llama.cpp` runtime integration scaffold.
- `crates/seceda-cli`: `seceda` CLI entrypoint.

`seceda-core` currently owns the portable tracer path: typed config defaults and
validation, runtime capability metadata, a runtime adapter trait, a deterministic
mock runtime, routing decisions, and observable execution results.

The initial router behavior is intentionally small:

- `local/default` forces the configured local runtime.
- `remote/default` forces the configured cloud runtime.
- `seceda/default` uses keyword routing to choose cloud for configured cloud
  keywords, otherwise it chooses the configured local runtime.

Run the minimal Rust checks from the repo root:

```bash
cargo test --workspace
```

Run the CLI scaffold:

```bash
cargo run -p seceda-cli -- --help
cargo run -p seceda-cli -- models
```

## Preserved Cloud Package

`seceda_cloud/` remains the standalone Python package for Modal and vLLM cloud
runtime work.

From the repo root:

```bash
uv sync --package seceda-cloud
uv run --package seceda-cloud modal run seceda_cloud/vllm_inference.py
```

From inside `seceda_cloud/`:

```bash
uv sync
uv run modal run vllm_inference.py
uv run modal deploy vllm_inference.py
```

## Preserved TUI Package

`seceda_tui/` remains the OpenTUI operator console package.

From inside `seceda_tui/`:

```bash
bun install
bun test
bun run check
bun run src/index.tsx
```

Useful environment overrides:

```bash
SECEDA_TUI_BASE_URL=http://127.0.0.1:8080
SECEDA_TUI_CONFIG_PATH=seceda_edge/config/seceda.toml
SECEDA_TUI_CATALOG_PATH=seceda_edge/config/config_catalog.toml
```

The config path defaults still point at the intended edge config location, even
though this transitional Rust scaffold has not recreated that config package.
