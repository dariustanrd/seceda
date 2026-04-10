#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PRESETS_FILE="${REPO_ROOT}/CMakePresets.json"

: "${CMAKE_EXECUTABLE:=cmake}"

if [[ ! -f "${PRESETS_FILE}" ]]; then
    echo "error: ${PRESETS_FILE} not found" >&2
    exit 1
fi

if [[ -t 1 ]]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    NC='\033[0m'
else
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    NC=''
fi

LOG_FILE=""
CMAKE_VERSION=""
HOST_OS="$(uname -s)"
HOST_ARCH="$(uname -m)"

PRESET="native-release"
CONFIGURE_ONLY=0
BUILD_ONLY=0
CLEAN=0
JOBS=""
LIST_ONLY=0
PYTHON_EXECUTABLE=""
BUILD_DIR=""

declare -a TARGETS=()
declare -a CMAKE_ARGS=()
declare -a BUILD_ARGS=()

usage() {
    cat <<'EOF'
Usage:
  scripts/build.sh [options]

Options:
  -p, --preset <name>      CMake preset to use (default: native-release)
      --python <path>      Python executable for ExecuTorch preflight/CMake
      --list-presets       Print presets available on this host and exit
      --configure-only     Run configure step only
      --build-only         Run build step only (requires existing build/<preset>)
  -c, --clean              Remove build/<preset> before configure/build
  -j, --jobs <N>           Parallel jobs for build step (passes --parallel N)
  -t, --target <name>      Build specific target (repeatable)
      --cmake-arg <arg>    Extra arg for configure command (repeatable)
      --build-arg <arg>    Extra arg for build command (repeatable)
  -h, --help               Show this help

Environment:
  CMAKE_EXECUTABLE         CMake executable (default: cmake)
  SECEDA_PYTHON            Python override for ExecuTorch preflight/CMake
  ARM_GNU_TOOLCHAIN_ROOT   Toolchain root for arm64 presets
  ARM_TARGET_TRIPLE        Cross compiler triple (default: aarch64-linux-gnu)
  ARM_SYSROOT              Optional sysroot for arm64 presets
  VULKAN_SDK               Vulkan SDK root containing bin/glslc

Examples:
  scripts/build.sh --list-presets
  scripts/build.sh --preset native-debug
  scripts/build.sh -p apple-silicon-release --clean
  scripts/build.sh -p arm64-release --configure-only
  scripts/build.sh -p native-release -t seceda_edge_daemon -j 8
  scripts/build.sh -p native-release --python "$(which python3)"
  scripts/build.sh -p apple-silicon-release --cmake-arg -DSECEDA_BUILD_EXECUTORCH=OFF

Output:
  Configure/build logs are saved to: build/<preset>/build_<timestamp>.log

Python for ExecuTorch:
  CMake and this script prefer (in order): -DPython3_EXECUTABLE=..., --python,
  SECEDA_PYTHON, ./.venv/bin/python3, ./.venv/bin/python, then python3 on PATH.
  Use `uv sync --group executorch-build` so the selected interpreter has torch.
EOF
}

log_to_file() {
    if [[ -n "${LOG_FILE}" ]]; then
        printf '%s\n' "$1" >> "${LOG_FILE}"
    fi
}

log_with_level() {
    local level="$1"
    local color="$2"
    local message="$3"

    printf '%b[%s]%b %s\n' "${color}" "${level}" "${NC}" "${message}"
    log_to_file "[${level}] ${message}"
}

log_info() {
    log_with_level "INFO" "${BLUE}" "$1"
}

log_success() {
    log_with_level "SUCCESS" "${GREEN}" "$1"
}

log_warning() {
    log_with_level "WARNING" "${YELLOW}" "$1"
}

log_error() {
    log_with_level "ERROR" "${RED}" "$1"
}

die() {
    log_error "$1"
    exit 1
}

setup_logging() {
    local build_dir="$1"
    local timestamp

    mkdir -p "${build_dir}"
    timestamp="$(date +"%Y%m%d_%H%M%S")"
    LOG_FILE="${build_dir}/build_${timestamp}.log"

    {
        echo "============================================================================="
        echo "Seceda Build Log"
        echo "============================================================================="
        echo "Timestamp: $(LC_ALL=C date)"
        echo "Host: ${HOST_OS} ${HOST_ARCH}"
        echo "Preset: ${PRESET}"
        echo "Build Directory: ${build_dir}"
        echo "============================================================================="
        echo
    } > "${LOG_FILE}"

    log_info "Logging to: ${LOG_FILE}"
}

log_section() {
    local title="$1"
    if [[ -n "${LOG_FILE}" ]]; then
        printf '\n=== %s ===\n' "${title}" >> "${LOG_FILE}"
    fi
}

run_logged() {
    if [[ -n "${LOG_FILE}" ]]; then
        set +e
        "$@" 2>&1 | tee -a "${LOG_FILE}"
        local exit_code=${PIPESTATUS[0]}
        set -e
        return "${exit_code}"
    fi

    "$@"
}

format_command() {
    local quoted=()
    local arg

    for arg in "$@"; do
        printf -v arg '%q' "${arg}"
        quoted+=("${arg}")
    done

    local IFS=' '
    printf '%s' "${quoted[*]}"
}

require_python3_for_metadata() {
    if ! command -v python3 >/dev/null 2>&1; then
        die "python3 is required to inspect CMakePresets.json."
    fi
}

check_cmake_version() {
    local version_output
    local major minor

    if ! version_output="$("${CMAKE_EXECUTABLE}" --version 2>/dev/null)"; then
        die "Failed to execute '${CMAKE_EXECUTABLE}'. Set CMAKE_EXECUTABLE to a CMake 3.29+ binary."
    fi

    CMAKE_VERSION="$(printf '%s\n' "${version_output}" | sed -n '1s/.* //p')"
    IFS=. read -r major minor _ <<< "${CMAKE_VERSION}"

    if [[ -z "${major:-}" || -z "${minor:-}" ]]; then
        die "Could not parse CMake version from '${CMAKE_EXECUTABLE} --version'."
    fi

    if (( major < 3 || (major == 3 && minor < 29) )); then
        die "CMake 3.29+ required, found ${CMAKE_VERSION}. Set CMAKE_EXECUTABLE to a newer binary."
    fi
}

list_presets() {
    if ! "${CMAKE_EXECUTABLE}" --list-presets; then
        die "Failed to list presets. Ensure CMake 3.29+ is installed and ${PRESETS_FILE} is valid."
    fi
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

for preset in data.get("configurePresets", []):
    if preset.get("name") == selected and not preset.get("hidden", False):
        sys.exit(0)

sys.exit(1)
PY
}

preset_available_on_host() {
    local selected="$1"
    local line

    while IFS= read -r line; do
        if [[ "${line}" =~ \"([^\"]+)\" ]] && [[ "${BASH_REMATCH[1]}" == "${selected}" ]]; then
            return 0
        fi
    done < <("${CMAKE_EXECUTABLE}" --list-presets 2>/dev/null)

    return 1
}

resolve_preset_cache_value() {
    local selected="$1"
    local key="$2"

    python3 - "${PRESETS_FILE}" "${selected}" "${key}" <<'PY'
import json
import sys

path, selected, key = sys.argv[1:4]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

presets = {p.get("name"): p for p in data.get("configurePresets", []) if p.get("name")}
visited = set()

def resolve_value(name):
    if not name or name in visited:
        return None

    visited.add(name)
    preset = presets.get(name, {})
    value = preset.get("cacheVariables", {}).get(key)
    if value is not None:
        if isinstance(value, bool):
            return "ON" if value else "OFF"
        return str(value)

    inherits = preset.get("inherits")
    if isinstance(inherits, list):
        for parent in inherits:
            result = resolve_value(parent)
            if result is not None:
                return result
        return None

    return resolve_value(inherits)

resolved = resolve_value(selected)
if resolved is not None:
    print(resolved)
PY
}

extract_cmake_arg_value() {
    local key="$1"
    shift

    local arg
    local value=""
    for arg in "$@"; do
        case "${arg}" in
            -D${key}=*|-D${key}:*=*)
                value="${arg#*=}"
                ;;
        esac
    done

    printf '%s' "${value}"
}

default_cmake_bool_for_cache_var() {
    case "$1" in
        SECEDA_BUILD_LLAMA_CPP|SECEDA_BUILD_EXECUTORCH|SECEDA_BUILD_CUSTOM|SECEDA_ENABLE_XNNPACK|SECEDA_ENABLE_OPENMP|SECEDA_EXECUTORCH_ENABLE_LLM|SECEDA_EXECUTORCH_ENABLE_QUANTIZED)
            printf 'ON'
            ;;
        SECEDA_ENABLE_VULKAN|SECEDA_ENABLE_METAL|SECEDA_LLAMA_ENABLE_KLEIDIAI|SECEDA_LLAMA_ENABLE_CURL|SECEDA_LLAMA_CPU_AARCH64|SECEDA_EXECUTORCH_ENABLE_APPLE|SECEDA_EXECUTORCH_ENABLE_METAL)
            printf 'OFF'
            ;;
        *)
            printf ''
            ;;
    esac
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

is_cache_var_enabled_for_run() {
    local selected="$1"
    local key="$2"
    shift 2

    local value
    value="$(resolve_preset_cache_value "${selected}" "${key}")"
    if [[ -z "${value}" ]]; then
        value="$(default_cmake_bool_for_cache_var "${key}")"
    fi

    local enabled=0
    if cmake_bool_is_true "${value}"; then
        enabled=1
    fi

    local override
    override="$(extract_cmake_arg_value "${key}" "$@")"
    if [[ -n "${override}" ]]; then
        if cmake_bool_is_true "${override}"; then
            enabled=1
        else
            enabled=0
        fi
    fi

    [[ "${enabled}" -eq 1 ]]
}

is_executorch_enabled_for_run() {
    is_cache_var_enabled_for_run "${PRESET}" "SECEDA_BUILD_EXECUTORCH" "${CMAKE_ARGS[@]}"
}

is_llama_enabled_for_run() {
    is_cache_var_enabled_for_run "${PRESET}" "SECEDA_BUILD_LLAMA_CPP" "${CMAKE_ARGS[@]}"
}

is_vulkan_enabled_for_run() {
    is_cache_var_enabled_for_run "${PRESET}" "SECEDA_ENABLE_VULKAN" "${CMAKE_ARGS[@]}"
}

is_apple_features_enabled_for_run() {
    is_cache_var_enabled_for_run "${PRESET}" "SECEDA_ENABLE_METAL" "${CMAKE_ARGS[@]}" \
        || is_cache_var_enabled_for_run "${PRESET}" "SECEDA_EXECUTORCH_ENABLE_APPLE" "${CMAKE_ARGS[@]}" \
        || is_cache_var_enabled_for_run "${PRESET}" "SECEDA_EXECUTORCH_ENABLE_METAL" "${CMAKE_ARGS[@]}"
}

resolve_default_python() {
    local explicit_python
    explicit_python="$(extract_cmake_arg_value "Python3_EXECUTABLE" "${CMAKE_ARGS[@]}")"
    if [[ -n "${explicit_python}" ]]; then
        printf '%s' "${explicit_python}"
        return
    fi

    if [[ -n "${PYTHON_EXECUTABLE}" ]]; then
        printf '%s' "${PYTHON_EXECUTABLE}"
        return
    fi

    if [[ -n "${SECEDA_PYTHON:-}" ]]; then
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

ensure_python_cmake_arg() {
    local resolved_python="$1"

    if [[ -n "$(extract_cmake_arg_value "Python3_EXECUTABLE" "${CMAKE_ARGS[@]}")" ]]; then
        return
    fi

    CMAKE_ARGS+=("-DPython3_EXECUTABLE=${resolved_python}")
}

python_version_string() {
    local py_exec="$1"

    "${py_exec}" - <<'PY'
import sys
print(f"{sys.version_info[0]}.{sys.version_info[1]}.{sys.version_info[2]}")
PY
}

check_python_version_ge_311() {
    local py_exec="$1"

    "${py_exec}" - <<'PY'
import sys
sys.exit(0 if sys.version_info >= (3, 11) else 1)
PY
}

check_python_has_torch() {
    local py_exec="$1"

    "${py_exec}" - <<'PY'
import importlib.util
import sys

sys.exit(0 if importlib.util.find_spec("torch") is not None else 1)
PY
}

validate_selected_preset() {
    if ! preset_exists "${PRESET}"; then
        echo "error: unknown preset '${PRESET}'" >&2
        echo "hint: run scripts/build.sh --list-presets" >&2
        list_presets >&2 || true
        exit 1
    fi

    if ! preset_available_on_host "${PRESET}"; then
        echo "error: preset '${PRESET}' is declared but unavailable on this host" >&2
        case "${PRESET}" in
            apple-silicon-*)
                echo "hint: apple-silicon presets are only exposed by CMake on macOS hosts" >&2
                ;;
            *)
                echo "hint: run scripts/build.sh --list-presets on this machine to see selectable presets" >&2
                ;;
        esac
        list_presets >&2 || true
        exit 1
    fi
}

check_required_submodules_for_run() {
    if is_llama_enabled_for_run && [[ ! -f "${REPO_ROOT}/thirdparty/llama.cpp/CMakeLists.txt" ]]; then
        die "llama.cpp submodule is missing. Run: git submodule update --init --recursive"
    fi

    if is_executorch_enabled_for_run && [[ ! -f "${REPO_ROOT}/thirdparty/executorch/CMakeLists.txt" ]]; then
        die "ExecuTorch submodule is missing. Run: git submodule update --init --recursive"
    fi
}

check_arm64_toolchain_preflight() {
    local target_triple="${ARM_TARGET_TRIPLE:-aarch64-linux-gnu}"
    local c_compiler=""
    local cxx_compiler=""

    log_info "Checking generic ARM64 toolchain (${target_triple})"

    if [[ -n "${ARM_GNU_TOOLCHAIN_ROOT:-}" ]]; then
        if [[ ! -d "${ARM_GNU_TOOLCHAIN_ROOT}" ]]; then
            die "ARM_GNU_TOOLCHAIN_ROOT not found at: ${ARM_GNU_TOOLCHAIN_ROOT}"
        fi

        c_compiler="${ARM_GNU_TOOLCHAIN_ROOT}/bin/${target_triple}-gcc"
        cxx_compiler="${ARM_GNU_TOOLCHAIN_ROOT}/bin/${target_triple}-g++"

        [[ -x "${c_compiler}" ]] || die "C compiler not found at ${c_compiler}. Check ARM_GNU_TOOLCHAIN_ROOT and ARM_TARGET_TRIPLE."
        [[ -x "${cxx_compiler}" ]] || die "CXX compiler not found at ${cxx_compiler}. Check ARM_GNU_TOOLCHAIN_ROOT and ARM_TARGET_TRIPLE."

        log_info "ARM_GNU_TOOLCHAIN_ROOT: ${ARM_GNU_TOOLCHAIN_ROOT}"
    else
        c_compiler="$(command -v "${target_triple}-gcc" 2>/dev/null || true)"
        cxx_compiler="$(command -v "${target_triple}-g++" 2>/dev/null || true)"

        if [[ -z "${c_compiler}" || -z "${cxx_compiler}" ]]; then
            die "Could not find ${target_triple}-gcc / ${target_triple}-g++ on PATH. Install a cross toolchain or set ARM_GNU_TOOLCHAIN_ROOT."
        fi
    fi

    if [[ -n "${ARM_SYSROOT:-}" ]]; then
        [[ -d "${ARM_SYSROOT}" ]] || die "ARM_SYSROOT not found at: ${ARM_SYSROOT}"
        log_info "ARM_SYSROOT: ${ARM_SYSROOT}"
    fi

    log_info "ARM C compiler: ${c_compiler}"
    log_info "ARM CXX compiler: ${cxx_compiler}"
}

check_vulkan_preflight() {
    local glslc_path=""

    log_info "Checking Vulkan shader compiler"

    if [[ -n "${VULKAN_SDK:-}" ]]; then
        if [[ ! -d "${VULKAN_SDK}" ]]; then
            die "VULKAN_SDK is set but not found at: ${VULKAN_SDK}"
        fi

        if [[ -x "${VULKAN_SDK}/bin/glslc" ]]; then
            glslc_path="${VULKAN_SDK}/bin/glslc"
        fi
    fi

    if [[ -z "${glslc_path}" ]]; then
        glslc_path="$(command -v glslc 2>/dev/null || true)"
    fi

    if [[ -z "${glslc_path}" ]]; then
        die "Vulkan support requires glslc. Install Vulkan shader tools or set VULKAN_SDK."
    fi

    log_info "GLSLC: ${glslc_path}"
}

check_executorch_python_preflight() {
    local py_exec="$1"
    local version

    if ! version="$(python_version_string "${py_exec}" 2>/dev/null)"; then
        die "Failed to execute Python interpreter '${py_exec}'."
    fi

    if ! check_python_version_ge_311 "${py_exec}"; then
        die "ExecuTorch requires Python 3.11+; found ${version} at ${py_exec}."
    fi

    if ! check_python_has_torch "${py_exec}"; then
        die "ExecuTorch is enabled for preset '${PRESET}', but Python cannot import torch. Run 'uv sync --group executorch-build', pass --python /path/to/python-with-torch, or use --cmake-arg -DSECEDA_BUILD_EXECUTORCH=OFF."
    fi

    ensure_python_cmake_arg "${py_exec}"
    log_info "Python3_EXECUTABLE: ${py_exec} (Python ${version})"
}

check_configure_preflight() {
    check_required_submodules_for_run

    if is_apple_features_enabled_for_run && [[ "${HOST_OS}" != "Darwin" ]]; then
        die "Apple-specific build options require macOS. Use apple-silicon-* presets on macOS or disable Metal/Apple cache variables."
    fi

    if [[ "${PRESET}" == arm64-* ]]; then
        check_arm64_toolchain_preflight
    fi

    if is_vulkan_enabled_for_run; then
        check_vulkan_preflight
    fi

    if is_executorch_enabled_for_run; then
        local resolved_python
        resolved_python="$(resolve_default_python)"

        if [[ -z "${resolved_python}" ]]; then
            die "No Python interpreter found (required for ExecuTorch builds). Create .venv with uv, pass --python /path/to/python3, or set SECEDA_PYTHON."
        fi

        check_executorch_python_preflight "${resolved_python}"
    fi
}

parse_args() {
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
}

validate_args() {
    if [[ "${CONFIGURE_ONLY}" -eq 1 && "${BUILD_ONLY}" -eq 1 ]]; then
        die "--configure-only and --build-only cannot be used together"
    fi

    if [[ "${BUILD_ONLY}" -eq 1 && "${#CMAKE_ARGS[@]}" -gt 0 ]]; then
        die "--cmake-arg cannot be used with --build-only because configure is skipped"
    fi

    if [[ "${BUILD_ONLY}" -eq 1 && -n "${PYTHON_EXECUTABLE}" ]]; then
        die "--python cannot be used with --build-only because configure is skipped"
    fi

    if [[ "${BUILD_ONLY}" -eq 1 && "${CLEAN}" -eq 1 ]]; then
        die "--clean cannot be used with --build-only because clean removes the configured build directory"
    fi

    if [[ "${CONFIGURE_ONLY}" -eq 1 && -n "${JOBS}" ]]; then
        die "--jobs cannot be used with --configure-only because build is skipped"
    fi

    if [[ "${CONFIGURE_ONLY}" -eq 1 && "${#TARGETS[@]}" -gt 0 ]]; then
        die "--target cannot be used with --configure-only because build is skipped"
    fi

    if [[ "${CONFIGURE_ONLY}" -eq 1 && "${#BUILD_ARGS[@]}" -gt 0 ]]; then
        die "--build-arg cannot be used with --configure-only because build is skipped"
    fi

    if [[ -n "${JOBS}" && ! "${JOBS}" =~ ^[0-9]+$ ]]; then
        die "--jobs must be a non-negative integer"
    fi

    local cmake_python
    cmake_python="$(extract_cmake_arg_value "Python3_EXECUTABLE" "${CMAKE_ARGS[@]}")"
    if [[ -n "${PYTHON_EXECUTABLE}" && -n "${cmake_python}" && "${PYTHON_EXECUTABLE}" != "${cmake_python}" ]]; then
        die "--python conflicts with --cmake-arg ${cmake_python}. Pick one Python override."
    fi
}

log_build_context() {
    log_info "=== seceda build ==="
    log_info "Project root: ${REPO_ROOT}"
    log_info "Host: ${HOST_OS} ${HOST_ARCH}"
    log_info "CMake: ${CMAKE_EXECUTABLE} (${CMAKE_VERSION})"
    log_info "Preset: ${PRESET}"
    log_info "Build directory: ${BUILD_DIR}"

    if [[ "${CLEAN}" -eq 1 ]]; then
        log_info "Clean build: yes"
    fi

    if [[ -n "${JOBS}" ]]; then
        log_info "Parallel jobs: ${JOBS}"
    fi

    if [[ "${#TARGETS[@]}" -gt 0 ]]; then
        log_info "Targets: ${TARGETS[*]}"
    fi

    if [[ "${#CMAKE_ARGS[@]}" -gt 0 ]]; then
        log_info "Extra configure args: $(format_command "${CMAKE_ARGS[@]}")"
    fi

    if [[ "${#BUILD_ARGS[@]}" -gt 0 ]]; then
        log_info "Extra build args: $(format_command "${BUILD_ARGS[@]}")"
    fi
}

run_configure() {
    declare -a configure_cmd=("${CMAKE_EXECUTABLE}" --preset "${PRESET}")
    if [[ "${#CMAKE_ARGS[@]}" -gt 0 ]]; then
        configure_cmd+=("${CMAKE_ARGS[@]}")
    fi

    log_info "Configuring preset: ${PRESET}"
    log_info "Running: $(format_command "${configure_cmd[@]}")"
    log_section "CMake Configure"
    run_logged "${configure_cmd[@]}"
}

run_build() {
    declare -a build_cmd=("${CMAKE_EXECUTABLE}" --build --preset "${PRESET}")
    local target

    if [[ -n "${JOBS}" ]]; then
        build_cmd+=(--parallel "${JOBS}")
    fi

    if [[ "${#TARGETS[@]}" -gt 0 ]]; then
        for target in "${TARGETS[@]}"; do
            build_cmd+=(--target "${target}")
        done
    fi

    if [[ "${#BUILD_ARGS[@]}" -gt 0 ]]; then
        build_cmd+=("${BUILD_ARGS[@]}")
    fi

    log_info "Building preset: ${PRESET}"
    log_info "Running: $(format_command "${build_cmd[@]}")"
    log_section "CMake Build"
    run_logged "${build_cmd[@]}"
}

main() {
    parse_args "$@"

    if [[ "${LIST_ONLY}" -eq 1 ]]; then
        check_cmake_version
        list_presets
        exit 0
    fi

    validate_args
    require_python3_for_metadata
    check_cmake_version
    validate_selected_preset

    BUILD_DIR="${REPO_ROOT}/build/${PRESET}"

    if [[ "${CLEAN}" -eq 1 ]]; then
        printf '==> Cleaning %s\n' "${BUILD_DIR}"
        rm -rf "${BUILD_DIR}"
    fi

    if [[ "${BUILD_ONLY}" -eq 1 && ! -d "${BUILD_DIR}" ]]; then
        die "build directory '${BUILD_DIR}' does not exist. Run configure first or remove --build-only."
    fi

    setup_logging "${BUILD_DIR}"
    log_build_context

    if [[ "${BUILD_ONLY}" -eq 0 ]]; then
        check_configure_preflight
        run_configure
    fi

    if [[ "${CONFIGURE_ONLY}" -eq 0 ]]; then
        if [[ ! -d "${BUILD_DIR}" ]]; then
            die "build directory '${BUILD_DIR}' does not exist. Run configure first or remove --build-only."
        fi

        run_build
        log_success "Build complete"
        log_info "Build artifacts: ${BUILD_DIR}"
    else
        log_success "Configuration complete"
    fi

    log_info "Build log: ${LOG_FILE}"
}

main "$@"
