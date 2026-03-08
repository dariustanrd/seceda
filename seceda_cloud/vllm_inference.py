"""Modal-hosted OpenAI-compatible vLLM server for Seceda."""

from __future__ import annotations

import asyncio
import json
import os
import shlex
import subprocess
import time
import urllib.error
import urllib.request
from typing import Any

import aiohttp
import modal
import modal.experimental

APP_NAME = "seceda-vllm-inference"
MODEL_NAME = os.getenv("SECEDA_VLLM_MODEL", "Qwen/Qwen3.5-4B")
MODEL_REVISION = os.getenv(
    "SECEDA_VLLM_REVISION",
    "851bf6e806efd8d0a36b00ddf55e13ccb7b8cd0a",
)
SERVED_MODEL_NAME = os.getenv("SECEDA_SERVED_MODEL", "seceda-cloud-default")
GPU_CONFIG = os.getenv("SECEDA_MODAL_GPU", "A100:1")
VLLM_PORT = int(os.getenv("SECEDA_VLLM_PORT", "8000"))
TARGET_INPUTS = int(os.getenv("SECEDA_TARGET_INPUTS", "32"))
MAX_INPUTS = int(os.getenv("SECEDA_MAX_INPUTS", "100"))
FAST_BOOT = os.getenv("SECEDA_FAST_BOOT", "false").lower() in {"1", "true", "yes"}

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
    modal.Image.from_registry("nvidia/cuda:12.8.0-devel-ubuntu22.04", add_python="3.12")
    .entrypoint([])
    .uv_pip_install(
        "vllm>=0.17.0",
        "huggingface-hub",
    )
    .env(
        {
            "HF_HUB_CACHE": "/root/.cache/huggingface",
            "HF_XET_HIGH_PERFORMANCE": "1",
            "VLLM_SERVER_DEV_MODE": "1",
        }
    )
)


def _localhost_url(path: str) -> str:
    return f"http://127.0.0.1:{VLLM_PORT}{path}"


def _request(
    path: str,
    *,
    method: str = "GET",
    json_body: dict[str, Any] | None = None,
    timeout_seconds: float = 60,
) -> bytes:
    data = None
    headers: dict[str, str] = {}
    if json_body is not None:
        data = json.dumps(json_body).encode("utf-8")
        headers["Content-Type"] = "application/json"

    request = urllib.request.Request(
        _localhost_url(path),
        data=data,
        headers=headers,
        method=method,
    )
    with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
        return response.read()


def _check_running(process: subprocess.Popen[str]) -> None:
    if (return_code := process.poll()) is not None:
        raise subprocess.CalledProcessError(return_code, process.args)


def _wait_until_ready(
    process: subprocess.Popen[str],
    timeout_seconds: int = SERVER_STARTUP_TIMEOUT_SECONDS,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            _check_running(process)
            _request("/health", timeout_seconds=10)
            return
        except (
            OSError,
            TimeoutError,
            subprocess.CalledProcessError,
            urllib.error.HTTPError,
            urllib.error.URLError,
        ):
            time.sleep(HEALTH_CHECK_INTERVAL_SECONDS)

    raise TimeoutError(f"vLLM server not ready within {timeout_seconds} seconds")


def _warm_up_server() -> None:
    payload = {
        "model": SERVED_MODEL_NAME,
        "messages": [{"role": "user", "content": "Hello, how are you?"}],
        "max_tokens": 16,
    }
    for _ in range(3):
        _request(
            "/v1/chat/completions",
            method="POST",
            json_body=payload,
            timeout_seconds=3 * MINUTES,
        )


def _sleep_server(level: int = 1) -> None:
    _request(f"/sleep?level={level}", method="POST", timeout_seconds=60)


def _wake_server() -> None:
    _request("/wake_up", method="POST", timeout_seconds=60)


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
    if FAST_BOOT:
        cmd.append("--enforce-eager")
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
        _wait_until_ready(self.process)
        _warm_up_server()
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
