"""Modal-hosted OpenAI-compatible SGLang server for Seceda."""

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
from seceda_cloud.sglang_compile import compile_deep_gemm_if_enabled


def _tensor_parallel_size_from_gpu_config(gpu_config: str) -> int:
    """Derive tensor-parallel size from Modal's GPU config string."""
    _gpu_name, separator, replica_count = gpu_config.rpartition(":")
    if not separator:
        return 1

    try:
        parsed = int(replica_count)
    except ValueError:
        return 1

    return max(parsed, 1)


def _default_tool_call_parser(model_name: str) -> str | None:
    if "lfm2" in model_name.lower():
        return "lfm2"
    return None


APP_NAME = "seceda-sglang-inference"
MODEL_NAME = os.getenv("SECEDA_SGLANG_MODEL", "LiquidAI/LFM2-24B-A2B")
MODEL_REVISION = os.getenv(
    "SECEDA_SGLANG_MODEL_REVISION",
    "a15c25dc8c1d348efd4d7859df744b74ce414f45",
)
SERVED_MODEL_NAME = os.getenv("SECEDA_SERVED_MODEL", "seceda-cloud-default")
GPU_CONFIG = os.getenv("SECEDA_MODAL_GPU", "H100:1")
# SGLANG_IMAGE_TAG = os.getenv(
#     "SECEDA_SGLANG_IMAGE_TAG",
#     "v0.5.9-cu130-amd64-runtime",
# )
SGLANG_PORT = int(os.getenv("SECEDA_SGLANG_PORT", "8000"))
TARGET_INPUTS = int(os.getenv("SECEDA_TARGET_INPUTS", "32"))
MAX_INPUTS = int(os.getenv("SECEDA_MAX_INPUTS", "100"))
ATTENTION_BACKEND = os.getenv("SECEDA_SGLANG_ATTENTION_BACKEND")
SPECULATIVE_DRAFT_MODEL = os.getenv("SECEDA_SGLANG_SPECULATIVE_DRAFT_MODEL")
# SPECULATIVE_ALGORITHM = os.getenv(
#     "SECEDA_SGLANG_SPECULATIVE_ALGORITHM",
#     "EAGLE3",
# )
SPECULATIVE_ALGORITHM = os.getenv("SECEDA_SGLANG_SPECULATIVE_ALGORITHM")
SPECULATIVE_NUM_STEPS = os.getenv("SECEDA_SGLANG_SPECULATIVE_NUM_STEPS")
SPECULATIVE_EAGLE_TOPK = os.getenv("SECEDA_SGLANG_SPECULATIVE_EAGLE_TOPK")
SPECULATIVE_NUM_DRAFT_TOKENS = os.getenv(
    "SECEDA_SGLANG_SPECULATIVE_NUM_DRAFT_TOKENS"
)
# TOOL_CALL_PARSER = os.getenv("SECEDA_SGLANG_TOOL_CALL_PARSER") or _default_tool_call_parser(
#     MODEL_NAME
# )
TOOL_CALL_PARSER = os.getenv("SECEDA_SGLANG_TOOL_CALL_PARSER")
MEM_FRACTION = os.getenv("SECEDA_SGLANG_MEM_FRACTION", "0.8")
DECODE_LOG_INTERVAL = os.getenv("SECEDA_SGLANG_DECODE_LOG_INTERVAL", "100")

REGION = os.getenv("SECEDA_MODAL_REGION", "us-east")
N_GPUS = _tensor_parallel_size_from_gpu_config(GPU_CONFIG)

MINUTES = 60
MIN_CONTAINERS = 0
HEALTH_CHECK_INTERVAL_SECONDS = 5
PROCESS_SHUTDOWN_TIMEOUT_SECONDS = 30
SERVER_STARTUP_TIMEOUT_SECONDS = 15 * MINUTES

HF_CACHE_PATH = "/root/.cache/huggingface-sglang"
DG_CACHE_PATH = "/root/.cache/deepgemm"

app = modal.App(APP_NAME)

hf_cache_volume = modal.Volume.from_name(
    "seceda-sglang-huggingface-cache",
    create_if_missing=True,
)
deepgemm_cache_volume = modal.Volume.from_name(
    "seceda-deepgemm-cache",
    create_if_missing=True,
)
HF_SECRET = modal.Secret.from_name("huggingface-secret")

cuda_version = "13.0.0"  # should be no greater than host CUDA version
flavor = "devel"  # includes full CUDA toolkit
operating_sys = "ubuntu24.04"
tag = f"{cuda_version}-{flavor}-{operating_sys}"

cuda_lib_paths = [
    "/usr/local/cuda/lib64",
    "/usr/local/nvidia/lib",
    "/usr/local/nvidia/lib64",
    "/usr/local/lib/python3.12/site-packages/nvidia/cuda_runtime/lib",
    "/usr/local/lib/python3.12/site-packages/nvidia/cuda_nvrtc/lib",
    "/usr/local/lib/python3.12/site-packages/nvidia/cublas/lib",
    "/usr/local/lib/python3.12/site-packages/nvidia/cudnn/lib",
    "/usr/local/lib/python3.12/site-packages/nvidia/cufft/lib",
    "/usr/local/lib/python3.12/site-packages/nvidia/curand/lib",
    "/usr/local/lib/python3.12/site-packages/nvidia/cusolver/lib",
    "/usr/local/lib/python3.12/site-packages/nvidia/cusparse/lib",
    "/usr/local/lib/python3.12/site-packages/nvidia/nccl/lib",
    "/usr/local/lib/python3.12/site-packages/nvidia/nvjitlink/lib",
]

sglang_image = (
    # modal.Image.from_registry(f"lmsysorg/sglang:{SGLANG_IMAGE_TAG}")
    modal.Image.from_registry(f"nvidia/cuda:{tag}", add_python="3.12")
    .entrypoint([])
    .apt_install("libnuma1", "git", "clang")
    .uv_pip_install(["torch==2.9.1", "torchvision", "torchaudio"],index_url="https://download.pytorch.org/whl/cu130")
    .uv_pip_install("sglang==0.5.9")
    .uv_pip_install("https://github.com/sgl-project/whl/releases/download/v0.3.21/sgl_kernel-0.3.21+cu130-cp312-abi3-manylinux2014_x86_64.whl")
    .uv_pip_install("huggingface-hub==1.6.0", "transformers==5.3.0", "wheel")
    .env(
        {
            "HF_HUB_CACHE": HF_CACHE_PATH,
            "HF_XET_HIGH_PERFORMANCE": "1",
            "SGLANG_ENABLE_JIT_DEEPGEMM": "1",
            "TORCHINDUCTOR_COMPILE_THREADS": "1",
            "PATH": (
                "/usr/local/cuda/bin:"
                "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
            ),
            "LD_LIBRARY_PATH": ":".join(cuda_lib_paths),
        }
    )
    .run_commands(
        "python -m pip uninstall -y torch_memory_saver torch-memory-saver || true",
        "rm -rf /tmp/torch_memory_saver",
        (
            "git clone https://github.com/fzyzcjy/torch_memory_saver.git "
            "/tmp/torch_memory_saver"
        ),
        (
            "bash -lc 'cd /tmp/torch_memory_saver && "
            "python -m pip install -v --no-build-isolation --no-deps .'"
        ),
    )
)


def compile_deep_gemm() -> None:
    """Precompile DeepGEMM kernels when the selected model/backend uses them."""
    compile_deep_gemm_if_enabled(
        model_name=MODEL_NAME,
        model_revision=MODEL_REVISION,
        tensor_parallel_size=N_GPUS,
    )


sglang_image = sglang_image.run_function(
    compile_deep_gemm,
    volumes={
        HF_CACHE_PATH: hf_cache_volume,
        DG_CACHE_PATH: deepgemm_cache_volume,
    },
    gpu=GPU_CONFIG,
)


def _sleep_server() -> None:
    request_bytes(
        SGLANG_PORT,
        "/release_memory_occupation",
        method="POST",
        json_body={},
        timeout_seconds=60,
    )


def _wake_server() -> None:
    request_bytes(
        SGLANG_PORT,
        "/resume_memory_occupation",
        method="POST",
        json_body={},
        timeout_seconds=60,
    )


def _build_sglang_command() -> list[str]:
    cmd = [
        "python",
        "-m",
        "sglang.launch_server",
        "--model-path",
        MODEL_NAME,
        "--served-model-name",
        SERVED_MODEL_NAME,
        "--host",
        "0.0.0.0",
        "--port",
        str(SGLANG_PORT),
        "--dtype",
        "bfloat16",
        "--tp",
        str(N_GPUS),
        "--cuda-graph-max-bs",
        str(MAX_INPUTS),
        "--max-running-requests",
        str(MAX_INPUTS),
        "--enable-metrics",
        "--enable-memory-saver",
        "--enable-weights-cpu-backup",
        "--decode-log-interval",
        DECODE_LOG_INTERVAL,
        "--mem-fraction",
        MEM_FRACTION,
    ]

    if MODEL_REVISION:
        cmd.extend(["--revision", MODEL_REVISION])
    if TOOL_CALL_PARSER:
        cmd.extend(["--tool-call-parser", TOOL_CALL_PARSER])
    if ATTENTION_BACKEND:
        cmd.extend(["--attention-backend", ATTENTION_BACKEND])
    if SPECULATIVE_DRAFT_MODEL:
        cmd.extend(
            [
                "--speculative-algorithm",
                SPECULATIVE_ALGORITHM,
                "--speculative-draft-model-path",
                SPECULATIVE_DRAFT_MODEL,
            ]
        )
    if SPECULATIVE_NUM_STEPS:
        cmd.extend(["--speculative-num-steps", SPECULATIVE_NUM_STEPS])
    if SPECULATIVE_EAGLE_TOPK:
        cmd.extend(["--speculative-eagle-topk", SPECULATIVE_EAGLE_TOPK])
    if SPECULATIVE_NUM_DRAFT_TOKENS:
        cmd.extend(
            [
                "--speculative-num-draft-tokens",
                SPECULATIVE_NUM_DRAFT_TOKENS,
            ]
        )

    return cmd


@app.cls(
    image=sglang_image,
    gpu=GPU_CONFIG,
    scaledown_window=3 * MINUTES,
    timeout=SERVER_STARTUP_TIMEOUT_SECONDS,
    volumes={
        HF_CACHE_PATH: hf_cache_volume,
        DG_CACHE_PATH: deepgemm_cache_volume,
    },
    secrets=[HF_SECRET],
    enable_memory_snapshot=True,
    experimental_options={"enable_gpu_snapshot": True},
    min_containers=MIN_CONTAINERS,
    region=REGION,
)
@modal.experimental.http_server(
    port=SGLANG_PORT,
    exit_grace_period=5,
    proxy_regions=[REGION],
)
@modal.concurrent(target_inputs=TARGET_INPUTS)
class SecedaSglangInference:
    """Expose a snapshot-backed SGLang server with an OpenAI-compatible API."""

    @modal.enter(snap=True)
    def startup(self) -> None:
        """Start, warm, and release SGLang before Modal captures the snapshot."""
        cmd = _build_sglang_command()

        print("LD_LIBRARY_PATH =", os.environ.get("LD_LIBRARY_PATH"))

        patterns = [
            "/usr/local/**/libcudart.so*",
            "/usr/local/lib/python3.12/site-packages/**/libcudart.so*",
            ]
        import glob
        for pattern in patterns:
            matches = glob.glob(pattern, recursive=True)
            print(pattern, matches)

        print(shlex.join(cmd))
        self.process = subprocess.Popen(cmd, env=os.environ.copy())
        wait_until_ready(
            self.process,
            port=SGLANG_PORT,
            label="SGLang",
            timeout_seconds=SERVER_STARTUP_TIMEOUT_SECONDS,
            health_check_interval_seconds=HEALTH_CHECK_INTERVAL_SECONDS,
        )
        warm_up_chat_server(
            port=SGLANG_PORT,
            model=SERVED_MODEL_NAME,
            timeout_seconds=3 * MINUTES,
        )
        _sleep_server()

    @modal.enter(snap=False)
    def restore(self) -> None:
        """Resume SGLang after restoring the Modal GPU snapshot."""
        _wake_server()

    @modal.exit()
    def stop(self) -> None:
        """Terminate the child SGLang process when the Modal container exits."""
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
    url = (await SecedaSglangInference._experimental_get_flash_urls.aio())[0]
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
    deployed_cls = modal.Cls.from_name(APP_NAME, "SecedaSglangInference")

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