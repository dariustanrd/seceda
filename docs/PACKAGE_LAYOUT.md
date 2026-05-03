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
mock runtime, heuristic routing decisions, and observable execution results.

The baseline heuristic router behavior is:

- `local/default` forces the configured local runtime.
- `remote/default` forces the configured cloud runtime.
- requests with tools, tool choice, or structured-output requirements route to
  cloud.
- prompts above the configured character or estimated token threshold route to
  cloud. The baseline defaults match the previous native router:
  `max_prompt_chars = 800` and `max_estimated_tokens = 256`.
- structured-output, freshness, and complexity keyword matches route to cloud.
- simple `seceda/default` requests choose the configured local runtime.

Router decisions include target, reason, matched rules, estimated prompt tokens,
and preferred runtime/model hints for later runtime selection. The estimated
token count uses the previous native heuristic: the larger of whitespace word
count and `ceil(character_count / 4)`.

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
