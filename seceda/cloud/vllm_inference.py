"""Modal-hosted OpenAI-compatible vLLM server for Seceda."""

from __future__ import annotations

import json
import os
import shlex
from typing import Any

import aiohttp
import modal

APP_NAME = "seceda-vllm-inference"
MODEL_NAME = os.getenv("SECEDA_VLLM_MODEL", "Qwen/Qwen3.5-4B")
MODEL_REVISION = os.getenv(
    "SECEDA_VLLM_REVISION",
    "851bf6e806efd8d0a36b00ddf55e13ccb7b8cd0a",
)
SERVED_MODEL_NAME = os.getenv("SECEDA_SERVED_MODEL", "seceda-cloud-default")
GPU_CONFIG = os.getenv("SECEDA_MODAL_GPU", "L40S:1")
VLLM_PORT = int(os.getenv("SECEDA_VLLM_PORT", "8000"))
MAX_CONCURRENT_INPUTS = int(os.getenv("SECEDA_MAX_CONCURRENT_INPUTS", "32"))
FAST_BOOT = os.getenv("SECEDA_FAST_BOOT", "true").lower() in {"1", "true", "yes"}

MINUTES = 60

app = modal.App(APP_NAME)

hf_cache_volume = modal.Volume.from_name("seceda-huggingface-cache", create_if_missing=True)
vllm_cache_volume = modal.Volume.from_name("seceda-vllm-cache", create_if_missing=True)
HF_SECRET = modal.Secret.from_name("huggingface-secret")

MIN_CONTAINERS=0

vllm_image = (
    modal.Image.from_registry("nvidia/cuda:12.8.0-devel-ubuntu22.04", add_python="3.12")
    .entrypoint([])
    .uv_pip_install(
        "vllm>=0.17.0",
        "huggingface-hub",
    )
    .env({"HF_XET_HIGH_PERFORMANCE": "1"})
)


@app.function(
    image=vllm_image,
    gpu=GPU_CONFIG,
    scaledown_window=15 * MINUTES,
    timeout=10 * MINUTES,
    volumes={
        "/root/.cache/huggingface": hf_cache_volume,
        "/root/.cache/vllm": vllm_cache_volume,
    },
    secrets=[HF_SECRET],
    # enable_memory_snapshot=True,
    # experimental_options={"enable_gpu_snapshot": True},   
)
@modal.concurrent(max_inputs=MAX_CONCURRENT_INPUTS)
@modal.web_server(port=VLLM_PORT, startup_timeout=10 * MINUTES)
def serve() -> None:
    """Expose a vLLM server with an OpenAI-compatible API."""
    import subprocess

    cmd = [
        "vllm",
        "serve",
        MODEL_NAME,
        "--revision",
        MODEL_REVISION,
        "--served-model-name",
        SERVED_MODEL_NAME,
        "--host",
        "0.0.0.0",
        "--port",
        str(VLLM_PORT),
        "--uvicorn-log-level",
        "info",
        "--tensor-parallel-size",
        "1",
    ]

    cmd.append("--enforce-eager" if FAST_BOOT else "--no-enforce-eager")

    print(shlex.join(cmd))
    subprocess.Popen(cmd)


@app.local_entrypoint()
async def smoke_test(
    prompt: str = "Explain why an edge-first inference router is useful.",
    test_timeout_minutes: int = 10,
    max_tokens: int = 128,
) -> None:
    """Run a health check and one chat completion against the deployed server."""

    url = await serve.get_web_url.aio()
    timeout_seconds = test_timeout_minutes * MINUTES
    messages = [{"role": "user", "content": prompt}]

    async with aiohttp.ClientSession(base_url=url) as session:
        async with session.get("/health", timeout=timeout_seconds) as response:
            response.raise_for_status()

        await _stream_chat_completion(
            session=session,
            model=SERVED_MODEL_NAME,
            messages=messages,
            timeout_seconds=timeout_seconds,
            max_tokens=max_tokens,
        )


async def _stream_chat_completion(
    session: aiohttp.ClientSession,
    model: str,
    messages: list[dict[str, str]],
    timeout_seconds: int,
    max_tokens: int,
) -> None:
    payload: dict[str, Any] = {
        "messages": messages,
        "model": model,
        "stream": True,
        "max_tokens": max_tokens,
    }
    headers = {
        "Content-Type": "application/json",
        "Accept": "text/event-stream",
    }

    async with session.post(
        "/v1/chat/completions",
        json=payload,
        headers=headers,
        timeout=timeout_seconds,
    ) as response:
        response.raise_for_status()

        async for raw in response.content:
            line = raw.decode().strip()
            if not line:
                continue
            if line.startswith("data: "):
                line = line[len("data: ") :]
            if line == "[DONE]":
                break

            chunk = json.loads(line)
            delta = chunk["choices"][0]["delta"]
            content = delta.get("content")
            if content:
                print(content, end="", flush=True)
    print()
