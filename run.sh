#!/usr/bin/env bash

set -euo pipefail

ACTION="${1:-run}"
BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build-linux"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "$ACTION" != "run" && "$ACTION" != "debug" ]]; then
  echo "Usage: $0 [run|debug]"
  exit 1
fi

cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -G Ninja
cmake --build "$BUILD_DIR" --parallel

TARGET="$BUILD_DIR/vulkan"
if [[ ! -x "$TARGET" ]]; then
  echo "Executable not found: $TARGET"
  exit 1
fi

if [[ "$ACTION" == "debug" ]]; then
  exec gdb "$TARGET"
else
  cd "$BUILD_DIR"
  exec "$TARGET"
fi
