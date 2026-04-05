from __future__ import annotations

import json
import subprocess
import time
import urllib.error
import urllib.request
from typing import Any


def localhost_url(port: int, path: str) -> str:
    return f"http://127.0.0.1:{port}{path}"


def request_bytes(
    port: int,
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
        localhost_url(port, path),
        data=data,
        headers=headers,
        method=method,
    )
    with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
        return response.read()


def check_running(process: subprocess.Popen[str]) -> None:
    if (return_code := process.poll()) is not None:
        raise subprocess.CalledProcessError(return_code, process.args)


def wait_until_ready(
    process: subprocess.Popen[str],
    *,
    port: int,
    label: str,
    timeout_seconds: int,
    health_check_interval_seconds: int = 5,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            check_running(process)
            request_bytes(port, "/health", timeout_seconds=10)
            return
        except (
            OSError,
            TimeoutError,
            subprocess.CalledProcessError,
            urllib.error.HTTPError,
            urllib.error.URLError,
        ):
            time.sleep(health_check_interval_seconds)

    raise TimeoutError(f"{label} server not ready within {timeout_seconds} seconds")


def warm_up_chat_server(
    *,
    port: int,
    model: str | None,
    timeout_seconds: float,
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
        request_bytes(
            port,
            "/v1/chat/completions",
            method="POST",
            json_body=payload,
            timeout_seconds=timeout_seconds,
        )
