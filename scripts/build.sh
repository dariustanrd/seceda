#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PRESETS_FILE="${REPO_ROOT}/CMakePresets.json"

if [[ ! -f "${PRESETS_FILE}" ]]; then
    echo "error: ${PRESETS_FILE} not found" >&2
    exit 1
fi

usage() {
    cat <<'EOF'
Usage:
  scripts/build.sh [options]

Options:
  -p, --preset <name>      CMake preset to use (default: native-release)
      --python <path>      Python executable for ExecuTorch/CMake preflight
      --list-presets       Print available non-hidden configure presets and exit
      --configure-only     Run configure step only
      --build-only         Run build step only (incompatible with --cmake-arg/--python)
  -c, --clean              Remove build/<preset> before configure/build
  -j, --jobs <N>           Parallel jobs for build step (passes --parallel N)
  -t, --target <name>      Build specific target (repeatable)
      --cmake-arg <arg>    Extra arg for configure command (repeatable)
      --build-arg <arg>    Extra arg for build command (repeatable)
  -h, --help               Show this help

Examples:
  scripts/build.sh --preset native-debug
  scripts/build.sh -p apple-silicon-release --clean
  scripts/build.sh -p arm64-release --configure-only
  scripts/build.sh -p native-release -t seceda_edge_daemon -j 8
  scripts/build.sh -p native-release --python "$(which python3)"

Python for ExecuTorch:
  By default, CMake and this script prefer (in order): --python, SECEDA_PYTHON,
  ./.venv/bin/python3, then python3 on PATH. Use `uv sync --group executorch-build`
  so the venv has torch before configuring with ExecuTorch enabled.
EOF
}

# Interpreter for ExecuTorch / CMake (matches CMakeLists.txt preference order).
resolve_default_python() {
    if [[ -n "${PYTHON_EXECUTABLE}" ]]; then
        printf '%s' "${PYTHON_EXECUTABLE}"
        return
    fi
    if [[ -n "${SECEDA_PYTHON:-}" ]] && [[ -x "${SECEDA_PYTHON}" ]]; then
        printf '%s' "${SECEDA_PYTHON}"
        return
    fi
    if [[ -x "${REPO_ROOT}/.venv/bin/python3" ]]; then
        printf '%s' "${REPO_ROOT}/.venv/bin/python3"
        return
    fi
    if [[ -x "${REPO_ROOT}/.venv/bin/python" ]]; then
        printf '%s' "${REPO_ROOT}/.venv/bin/python"
        return
    fi
    command -v python3 2>/dev/null || true
}

list_presets() {
    python3 - "${PRESETS_FILE}" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

presets = data.get("configurePresets", [])
names = [p.get("name") for p in presets if p.get("name") and not p.get("hidden", False)]
for name in names:
    print(name)
PY
}

preset_exists() {
    local selected="$1"
    python3 - "${PRESETS_FILE}" "${selected}" <<'PY'
import json
import sys

path = sys.argv[1]
selected = sys.argv[2]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

for p in data.get("configurePresets", []):
    if p.get("name") == selected and not p.get("hidden", False):
        sys.exit(0)

sys.exit(1)
PY
}

is_executorch_enabled_for_preset() {
    local selected="$1"
    python3 - "${PRESETS_FILE}" "${selected}" <<'PY'
import json
import sys

path = sys.argv[1]
selected = sys.argv[2]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

presets = {p.get("name"): p for p in data.get("configurePresets", []) if p.get("name")}

def resolve_bool(name, key):
    visited = set()
    while name and name not in visited:
        visited.add(name)
        p = presets.get(name, {})
        val = p.get("cacheVariables", {}).get(key)
        if val is not None:
            if isinstance(val, bool):
                return val
            if isinstance(val, str):
                return val.upper() in ("ON", "TRUE", "1")
            return bool(val)
        inherits = p.get("inherits")
        if isinstance(inherits, list):
            name = inherits[0] if inherits else None
        else:
            name = inherits
    return None

enabled = resolve_bool(selected, "SECEDA_BUILD_EXECUTORCH")
if enabled is None:
    enabled = True

sys.exit(0 if enabled else 1)
PY
}

cmake_bool_is_true() {
    local normalized
    normalized="$(printf '%s' "${1:-}" | tr '[:lower:]' '[:upper:]')"
    case "${normalized}" in
        ON|TRUE|1|YES|Y)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

is_executorch_enabled_for_run() {
    local selected="$1"
    shift

    local enabled=0
    if is_executorch_enabled_for_preset "${selected}"; then
        enabled=1
    fi

    local arg value
    for arg in "$@"; do
        case "${arg}" in
            -DSECEDA_BUILD_EXECUTORCH=*|-DSECEDA_BUILD_EXECUTORCH:*=*)
                value="${arg#*=}"
                if cmake_bool_is_true "${value}"; then
                    enabled=1
                else
                    enabled=0
                fi
                ;;
        esac
    done

    [[ "${enabled}" -eq 1 ]]
}

check_python_has_torch() {
    local py_exec="$1"
    "${py_exec}" - <<'PY'
import importlib.util
import sys
spec = importlib.util.find_spec("torch")
sys.exit(0 if spec is not None else 1)
PY
}

PRESET="native-release"
CONFIGURE_ONLY=0
BUILD_ONLY=0
CLEAN=0
JOBS=""
LIST_ONLY=0
PYTHON_EXECUTABLE=""

declare -a TARGETS=()
declare -a CMAKE_ARGS=()
declare -a BUILD_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--preset)
            [[ $# -lt 2 ]] && { echo "error: --preset requires a value" >&2; exit 1; }
            PRESET="$2"
            shift 2
            ;;
        --list-presets)
            LIST_ONLY=1
            shift
            ;;
        --python)
            [[ $# -lt 2 ]] && { echo "error: --python requires a value" >&2; exit 1; }
            PYTHON_EXECUTABLE="$2"
            shift 2
            ;;
        --configure-only)
            CONFIGURE_ONLY=1
            shift
            ;;
        --build-only)
            BUILD_ONLY=1
            shift
            ;;
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -j|--jobs)
            [[ $# -lt 2 ]] && { echo "error: --jobs requires a value" >&2; exit 1; }
            JOBS="$2"
            shift 2
            ;;
        -t|--target)
            [[ $# -lt 2 ]] && { echo "error: --target requires a value" >&2; exit 1; }
            TARGETS+=("$2")
            shift 2
            ;;
        --cmake-arg)
            [[ $# -lt 2 ]] && { echo "error: --cmake-arg requires a value" >&2; exit 1; }
            CMAKE_ARGS+=("$2")
            shift 2
            ;;
        --build-arg)
            [[ $# -lt 2 ]] && { echo "error: --build-arg requires a value" >&2; exit 1; }
            BUILD_ARGS+=("$2")
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option '$1'" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ "${LIST_ONLY}" -eq 1 ]]; then
    list_presets
    exit 0
fi

if [[ "${CONFIGURE_ONLY}" -eq 1 && "${BUILD_ONLY}" -eq 1 ]]; then
    echo "error: --configure-only and --build-only cannot be used together" >&2
    exit 1
fi

if [[ "${BUILD_ONLY}" -eq 1 && "${#CMAKE_ARGS[@]}" -gt 0 ]]; then
    echo "error: --cmake-arg cannot be used with --build-only because configure is skipped" >&2
    echo "hint: remove --build-only, or re-run configure first with the same --cmake-arg values" >&2
    exit 1
fi

if [[ "${BUILD_ONLY}" -eq 1 && -n "${PYTHON_EXECUTABLE}" ]]; then
    echo "error: --python cannot be used with --build-only because configure is skipped" >&2
    echo "hint: remove --build-only, or re-run configure first with --python" >&2
    exit 1
fi

if [[ -n "${JOBS}" && ! "${JOBS}" =~ ^[0-9]+$ ]]; then
    echo "error: --jobs must be a non-negative integer" >&2
    exit 1
fi

if ! preset_exists "${PRESET}"; then
    echo "error: unknown preset '${PRESET}'" >&2
    echo "available presets:" >&2
    list_presets | sed 's/^/  - /' >&2
    exit 1
fi

BUILD_DIR="${REPO_ROOT}/build/${PRESET}"

if [[ "${CLEAN}" -eq 1 ]]; then
    echo "==> Cleaning ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

if [[ "${BUILD_ONLY}" -eq 0 ]]; then
    executorch_enabled_for_run=0
    if is_executorch_enabled_for_run "${PRESET}" "${CMAKE_ARGS[@]}"; then
        executorch_enabled_for_run=1
    fi

    local_python="$(resolve_default_python)"
    if [[ "${executorch_enabled_for_run}" -eq 1 && -n "${local_python}" ]]; then
        CMAKE_ARGS+=("-DPython3_EXECUTABLE=${local_python}")
    fi

    if [[ "${executorch_enabled_for_run}" -eq 1 ]]; then
        if [[ -z "${local_python}" ]]; then
            echo "error: no Python interpreter found (required for ExecuTorch builds)." >&2
            echo "hint: create .venv with uv, pass --python /path/to/python3, or set SECEDA_PYTHON" >&2
            exit 1
        fi
        if ! check_python_has_torch "${local_python}"; then
            echo "error: ExecuTorch is enabled for preset '${PRESET}', but Python cannot import torch." >&2
            echo "python checked: ${local_python}" >&2
            echo "hint: uv sync --group executorch-build  (repo root), or pass --python /path/to/python-with-torch" >&2
            echo "hint: to skip ExecuTorch for this run, use --cmake-arg -DSECEDA_BUILD_EXECUTORCH=OFF" >&2
            exit 1
        fi
    fi

    echo "==> Configuring preset: ${PRESET}"
    declare -a CONFIGURE_CMD=(cmake --preset "${PRESET}")
    if [[ "${#CMAKE_ARGS[@]:-0}" -gt 0 ]]; then
        CONFIGURE_CMD+=("${CMAKE_ARGS[@]-}")
    fi
    "${CONFIGURE_CMD[@]}"
fi

if [[ "${CONFIGURE_ONLY}" -eq 0 ]]; then
    if [[ ! -d "${BUILD_DIR}" ]]; then
        echo "error: build directory '${BUILD_DIR}' does not exist." >&2
        echo "Run configure first (remove --build-only), or use --clean without --build-only." >&2
        exit 1
    fi

    declare -a BUILD_CMD=(cmake --build --preset "${PRESET}")

    if [[ -n "${JOBS}" ]]; then
        BUILD_CMD+=(--parallel "${JOBS}")
    fi

    for target in "${TARGETS[@]-}"; do
        BUILD_CMD+=(--target "${target}")
    done

    if [[ "${#BUILD_ARGS[@]:-0}" -gt 0 ]]; then
        BUILD_CMD+=("${BUILD_ARGS[@]-}")
    fi

    echo "==> Building preset: ${PRESET}"
    "${BUILD_CMD[@]}"
fi
