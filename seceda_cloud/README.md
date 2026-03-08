# seceda-cloud

This area contains the standalone cloud Python project for Seceda's Modal and
vLLM serving scaffold.

Use this project's own `pyproject.toml` for standalone setup:
`uv sync`

Run or deploy the Modal app with:
`uv run modal run vllm_inference.py`
or
`uv run modal deploy vllm_inference.py`

From the repo root workspace, use:
`uv sync --package seceda-cloud`
and
`uv run --package seceda-cloud modal run seceda_cloud/vllm_inference.py`
