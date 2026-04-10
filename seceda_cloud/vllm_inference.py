"""Modal-hosted OpenAI-compatible vLLM server for Seceda."""

from __future__ import annotations

import asyncio
import json
import os
import shlex
import subprocess
import time
from typing import Any

import aiohttp
import modal
import modal.experimental

from seceda_cloud.local_server_utils import (
    request_bytes,
    wait_until_ready,
    warm_up_chat_server,
)

APP_NAME = "seceda-vllm-inference"
MODEL_NAME = os.getenv("SECEDA_VLLM_MODEL", "LiquidAI/LFM2-24B-A2B")
MODEL_REVISION = os.getenv(
    "SECEDA_VLLM_MODEL_REVISION",
    "a15c25dc8c1d348efd4d7859df744b74ce414f45",
)
SERVED_MODEL_NAME = os.getenv("SECEDA_SERVED_MODEL", "seceda-cloud-default")
GPU_CONFIG = os.getenv("SECEDA_MODAL_GPU", "H100:1")
VLLM_PORT = int(os.getenv("SECEDA_VLLM_PORT", "8000"))
TARGET_INPUTS = int(os.getenv("SECEDA_TARGET_INPUTS", "32"))
MAX_INPUTS = int(os.getenv("SECEDA_MAX_INPUTS", "100"))
# FAST_BOOT = os.getenv("SECEDA_FAST_BOOT", "false").lower() in {"1", "true", "yes"}

REGION = os.getenv("SECEDA_MODAL_REGION", "us-east")

MINUTES = 60
MIN_CONTAINERS = 0
HEALTH_CHECK_INTERVAL_SECONDS = 5
PROCESS_SHUTDOWN_TIMEOUT_SECONDS = 30
SERVER_STARTUP_TIMEOUT_SECONDS = 15 * MINUTES

app = modal.App(APP_NAME)

hf_cache_volume = modal.Volume.from_name("seceda-huggingface-cache", create_if_missing=True)
vllm_cache_volume = modal.Volume.from_name("seceda-vllm-cache", create_if_missing=True)
HF_SECRET = modal.Secret.from_name("huggingface-secret")

vllm_image = (
    modal.Image.from_registry("vllm/vllm-openai:v0.15.1")
    .entrypoint([])
    .run_commands("ln -s $(which python3) /usr/bin/python")
    .pip_install("transformers==5.1.0")
    .env(
        {
            "HF_HUB_CACHE": "/root/.cache/huggingface",
            "HF_XET_HIGH_PERFORMANCE": "1",
            "VLLM_SERVER_DEV_MODE": "1",
            "TORCH_CPP_LOG_LEVEL": "FATAL",
            "MODEL_NAME": MODEL_NAME,
        }
    )
)


def _sleep_server(level: int = 1) -> None:
    request_bytes(VLLM_PORT, f"/sleep?level={level}", method="POST", timeout_seconds=60)


def _wake_server() -> None:
    request_bytes(VLLM_PORT, "/wake_up", method="POST", timeout_seconds=60)


def _build_vllm_command() -> list[str]:
    cmd = [
        "vllm",
        "serve",
        MODEL_NAME,
        "--served-model-name",
        SERVED_MODEL_NAME,
        "--host",
        "0.0.0.0",
        "--port",
        str(VLLM_PORT),
        "--dtype",
        "bfloat16",
        "--gpu-memory-utilization",
        "0.8",
        "--uvicorn-log-level",
        "info",
        "--max-num-seqs",
        f"{MAX_INPUTS}",
        "--max-cudagraph-capture-size",
        f"{MAX_INPUTS}",
        "--enable-sleep-mode",
    ]
    if MODEL_REVISION:
        cmd.extend(["--revision", MODEL_REVISION])
    # if FAST_BOOT:
    #     cmd.append("--enforce-eager")
    return cmd


@app.cls(
    image=vllm_image,
    gpu=GPU_CONFIG,
    scaledown_window=3 * MINUTES,
    timeout=SERVER_STARTUP_TIMEOUT_SECONDS,
    volumes={
        "/root/.cache/huggingface": hf_cache_volume,
        "/root/.cache/vllm": vllm_cache_volume,
    },
    secrets=[HF_SECRET],
    enable_memory_snapshot=True,
    experimental_options={"enable_gpu_snapshot": True},
    min_containers=MIN_CONTAINERS,
    region=REGION,
)
@modal.experimental.http_server(
    port=VLLM_PORT, 
    exit_grace_period=5,
    proxy_regions=[REGION],
    )
@modal.concurrent(
    target_inputs=TARGET_INPUTS,
)
class SecedaVllmInference:
    """Expose a snapshot-backed vLLM server with an OpenAI-compatible API."""

    @modal.enter(snap=True)
    def startup(self) -> None:
        """Start, warm, and sleep vLLM before Modal captures the memory snapshot."""
        cmd = _build_vllm_command()
        print(shlex.join(cmd))
        self.process = subprocess.Popen(cmd)
        wait_until_ready(
            self.process,
            port=VLLM_PORT,
            label="vLLM",
            timeout_seconds=SERVER_STARTUP_TIMEOUT_SECONDS,
            health_check_interval_seconds=HEALTH_CHECK_INTERVAL_SECONDS,
        )
        warm_up_chat_server(
            port=VLLM_PORT,
            model=SERVED_MODEL_NAME,
            timeout_seconds=3 * MINUTES,
        )
        _sleep_server()

    @modal.enter(snap=False)
    def restore(self) -> None:
        """Wake the restored vLLM server so it can accept traffic immediately."""
        _wake_server()

    @modal.exit()
    def stop(self) -> None:
        """Terminate the child vLLM process when the Modal container exits."""
        if not hasattr(self, "process") or self.process.poll() is not None:
            return

        self.process.terminate()
        try:
            self.process.wait(timeout=PROCESS_SHUTDOWN_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()


@app.local_entrypoint()
async def smoke_test(
    prompt: str = "Explain why an edge-first inference router is useful.",
    test_timeout_minutes: int = 10,
    max_tokens: int = 128,
) -> None:
    """Run a health check and one chat completion against the deployed server."""
    url = (await SecedaVllmInference._experimental_get_flash_urls.aio())[0]
    timeout_seconds = test_timeout_minutes * MINUTES
    messages = [{"role": "user", "content": prompt}]

    await _probe_server(
        url=url,
        model=SERVED_MODEL_NAME,
        messages=messages,
        timeout_seconds=timeout_seconds,
        max_tokens=max_tokens,
    )


async def _probe_server(
    *,
    url: str,
    model: str,
    messages: list[dict[str, str]],
    timeout_seconds: int,
    max_tokens: int,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    headers = {"Modal-Session-ID": "seceda-smoke-test"}

    async with aiohttp.ClientSession(base_url=url, headers=headers) as session:
        while time.monotonic() < deadline:
            remaining_seconds = max(1.0, deadline - time.monotonic())
            try:
                async with session.get("/health", timeout=remaining_seconds) as response:
                    response.raise_for_status()

                await _stream_chat_completion(
                    session=session,
                    model=model,
                    messages=messages,
                    timeout_seconds=remaining_seconds,
                    max_tokens=max_tokens,
                )
                return
            except asyncio.TimeoutError:
                await asyncio.sleep(1)
            except aiohttp.ClientResponseError as exc:
                if exc.status in {502, 503, 504}:
                    await asyncio.sleep(1)
                    continue
                raise
            except aiohttp.ClientConnectionError:
                await asyncio.sleep(1)

    raise TimeoutError(f"No response from server within {timeout_seconds} seconds")


async def _stream_chat_completion(
    session: aiohttp.ClientSession,
    model: str,
    messages: list[dict[str, str]],
    timeout_seconds: float,
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
            line = raw.decode("utf-8", errors="ignore").strip()
            if not line or not line.startswith("data:"):
                continue

            data = line[len("data:") :].strip()
            if data == "[DONE]":
                break

            try:
                chunk = json.loads(data)
            except json.JSONDecodeError:
                continue

            delta = (chunk.get("choices") or [{}])[0].get("delta") or {}
            content = delta.get("content")
            if content:
                print(content, end="", flush="\n" in content or "." in content)
    print()


if __name__ == "__main__":
    deployed_cls = modal.Cls.from_name(APP_NAME, "SecedaVllmInference")

    async def main() -> None:
        url = (await deployed_cls._experimental_get_flash_urls.aio())[0]
        messages = [{"role": "user", "content": "Tell me ten jokes."}]
        await _probe_server(
            url=url,
            model=SERVED_MODEL_NAME,
            messages=messages,
            timeout_seconds=10 * MINUTES,
            max_tokens=128,
        )

    try:
        print("calling inference server")
        asyncio.run(main())
    except modal.exception.NotFoundError as exc:
        raise Exception(
            f"To take advantage of GPU snapshots, deploy first with modal deploy {__file__}"
        ) from exc
