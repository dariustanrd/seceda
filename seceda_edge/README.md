# seceda-edge

This area contains the standalone edge Python project for Seceda's helpers,
experiments, and tooling.

Use this project's own `pyproject.toml` for standalone setup:
`uv sync`

Run the example client with:
`uv run python test_client.py`

From the repo root workspace, use:
`uv sync --package seceda-edge`
and
`uv run --package seceda-edge python seceda_edge/test_client.py`
