from __future__ import annotations

import os
import subprocess


def compile_deep_gemm_if_enabled(
    *,
    model_name: str,
    model_revision: str | None,
    tensor_parallel_size: int,
    env_var: str = "SGLANG_ENABLE_JIT_DEEPGEMM",
) -> None:
    if os.environ.get(env_var, "1").lower() not in {"1", "true", "yes", "on"}:
        return

    command = [
        "python3",
        "-m",
        "sglang.compile_deep_gemm",
        "--model-path",
        model_name,
        "--tp",
        str(tensor_parallel_size),
    ]
    if model_revision:
        command.extend(["--revision", model_revision])

    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        print("DeepGEMM precompile exited non-zero; runtime compilation will be used if needed.")
