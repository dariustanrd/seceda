from __future__ import annotations

import subprocess
import time
from typing import Any


def check_running(process: subprocess.Popen) -> None:
    if (return_code := process.poll()) is not None:
        raise subprocess.CalledProcessError(return_code, cmd=process.args)


def wait_ready(
    process: subprocess.Popen,
    *,
    requests_module: Any,
    port: int,
    label: str,
    timeout: int,
    poll_interval_seconds: int = 5,
) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            check_running(process)
            requests_module.get(f"http://127.0.0.1:{port}/health").raise_for_status()
            return
        except (
            subprocess.CalledProcessError,
            requests_module.exceptions.ConnectionError,
            requests_module.exceptions.HTTPError,
        ):
            time.sleep(poll_interval_seconds)
    raise TimeoutError(f"{label} server not ready within {timeout} seconds")


def warmup_chat_server(
    *,
    requests_module: Any,
    port: int,
    model: str | None,
    timeout: int,
    repetitions: int = 3,
    prompt: str = "Hello, how are you?",
    max_tokens: int = 16,
) -> None:
    payload: dict[str, Any] = {
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
    }
    if model is not None:
        payload["model"] = model

    for _ in range(repetitions):
        requests_module.post(
            f"http://127.0.0.1:{port}/v1/chat/completions",
            json=payload,
            timeout=timeout,
        ).raise_for_status()


def sleep_vllm_server(*, requests_module: Any, port: int, level: int = 1) -> None:
    requests_module.post(f"http://127.0.0.1:{port}/sleep?level={level}").raise_for_status()


def wake_vllm_server(*, requests_module: Any, port: int) -> None:
    requests_module.post(f"http://127.0.0.1:{port}/wake_up").raise_for_status()


def sleep_sglang_server(*, requests_module: Any, port: int) -> None:
    requests_module.post(
        f"http://127.0.0.1:{port}/release_memory_occupation",
        json={},
    ).raise_for_status()


def wake_sglang_server(*, requests_module: Any, port: int) -> None:
    requests_module.post(
        f"http://127.0.0.1:{port}/resume_memory_occupation",
        json={},
    ).raise_for_status()
