#!/usr/bin/env bash

set -euo pipefail

if [[ "${OS:-}" == "Windows_NT" ]]; then
    echo "This script must be run in a Unix environment (WSL/Linux/macOS)." >&2
    exit 1
fi

detect_clang_format_bin() {
    if [[ -n "${CLANG_FORMAT_BIN:-}" && -x "${CLANG_FORMAT_BIN}" ]]; then
        printf '%s\n' "${CLANG_FORMAT_BIN}"
        return 0
    fi

    if [[ "$(uname -s)" == "Darwin" ]]; then
        if [[ -x "/opt/homebrew/opt/llvm@18/bin/clang-format" ]]; then
            printf '%s\n' "/opt/homebrew/opt/llvm@18/bin/clang-format"
            return 0
        fi

        if ! command -v brew >/dev/null 2>&1; then
            echo "Homebrew not found. Please install Homebrew first, then rerun this script." >&2
            return 1
        fi

        echo "llvm@18 not found, installing with Homebrew..." >&2
        brew install llvm@18

        if [[ -x "/opt/homebrew/opt/llvm@18/bin/clang-format" ]]; then
            printf '%s\n' "/opt/homebrew/opt/llvm@18/bin/clang-format"
            return 0
        fi
    fi

    if command -v clang-format >/dev/null 2>&1; then
        printf '%s\n' "$(command -v clang-format)"
        return 0
    fi

    echo "clang-format not found in PATH." >&2
    return 1
}

clang_format_bin="$(detect_clang_format_bin)"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"

targets=("$@")
if [[ ${#targets[@]} -eq 0 ]]; then
    targets=("src")
fi

files=()
for target in "${targets[@]}"; do
    while IFS= read -r -d '' file; do
        files+=("$file")
    done < <(find "$target" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) -print0)
done

if [[ ${#files[@]} -eq 0 ]]; then
    echo "No C/C++ source files found." >&2
    exit 1
fi

"$clang_format_bin" -i "${files[@]}"
echo "Formatted ${#files[@]} file(s) with clang-format."
