# seceda-cloud

This area contains the Modal deployment and vLLM serving scaffold for Seceda.

Use the repo-root `pyproject.toml` to create the local environment:
`uv sync --extra cloud`

Run or deploy the Modal app with:
`uv run --extra cloud modal run seceda/cloud/vllm_inference.py`
or
`uv run --extra cloud modal deploy seceda/cloud/vllm_inference.py`
