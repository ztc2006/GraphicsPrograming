#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_ROOT/build-linux"
BUILD=true
BUILD_ONLY=false
DEBUG=false
SCENE=""
JOBS=""
VIEWER_ARGS=()
CMAKE_OPTIONS=()

usage() {
  cat <<EOF
Usage: $0 [--no-build] [--debug] [--build-only] [-j jobs] [scene.gltf|scene.glb|scene.obj]

Default: configure and incrementally build, then open the model viewer.
Choose a model in the Scene panel, drop a file, or pass a model path here.
Relative model paths are resolved from the caller's current directory.
  --no-build     Run the existing binary (fails if it is missing).
  --build-only   Build without opening a window.
  --debug        Run under gdb.
  --build        Explicitly request the default build behavior.
  --release      Configure an optimized Release build.
  --benchmark DIR --warmup S --duration S --size WIDTHxHEIGHT
  --frames-in-flight 1|2 --present-sync auto|fence|legacy
  --light-culling full|clustered --lighting-preset auto|asset|kitchen
  --camera-path static|orbit --present auto|fifo|mailbox|immediate
  --gpu NAME --no-ui --validation are forwarded to the viewer.
EOF
}
fail() { echo "Error: $*" >&2; exit 1; }
while (($#)); do
  case "$1" in
    -b|--build) BUILD=true ;;
    --no-build) BUILD=false ;;
    --release) CMAKE_OPTIONS+=(-DCMAKE_BUILD_TYPE=Release) ;;
    --benchmark)
      (($# >= 2)) || fail "$1 requires an output directory"
      VIEWER_ARGS+=("$1" "$(realpath -m -- "$2")"); shift ;;
    --warmup|--duration|--size|--camera-path|--light-culling|--lighting-preset|--present|--gpu|--frames-in-flight|--present-sync)
      (($# >= 2)) || fail "$1 requires a value"
      VIEWER_ARGS+=("$1" "$2"); shift ;;
    --no-ui|--validation) VIEWER_ARGS+=("$1") ;;
    --build-only|build) BUILD=true; BUILD_ONLY=true ;;
    -d|--debug|debug) DEBUG=true ;;
    run) ;;
    -j|--jobs)
      (($# >= 2)) || fail "$1 requires a positive integer"
      JOBS="$2"; shift
      [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "jobs must be a positive integer"
      ;;
    -h|--help) usage; exit 0 ;;
    --) shift; (($# <= 1)) || fail "Only one model may be loaded"; if (($#)); then SCENE="$1"; shift; fi; break ;;
    -*) fail "Unknown option: $1" ;;
    *) [[ -z "$SCENE" ]] || fail "Only one model may be loaded"; SCENE="$1" ;;
  esac
  shift
done
if [[ -n "$SCENE" ]]; then
  [[ -f "$SCENE" ]] || fail "Scene file not found: $SCENE"
  case "${SCENE,,}" in *.gltf|*.glb|*.obj) ;; *) fail "Supported model formats: glTF, GLB, OBJ" ;; esac
  SCENE="$(realpath -- "$SCENE")"
fi
[[ "$BUILD_ONLY" != true || "$DEBUG" != true ]] || fail "--debug cannot be combined with --build-only"
if [[ "$BUILD" == true ]]; then
  command -v cmake >/dev/null || fail "cmake is required"
  cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -G Ninja "${CMAKE_OPTIONS[@]}"
  if [[ -n "$JOBS" ]]; then cmake --build "$BUILD_DIR" --parallel "$JOBS"
  else cmake --build "$BUILD_DIR" --parallel; fi
fi
[[ "$BUILD_ONLY" != true ]] || exit 0
TARGET="$BUILD_DIR/vulkan"
[[ -x "$TARGET" ]] || fail "Executable missing: $TARGET. Run without --no-build first."
ARGS=("${VIEWER_ARGS[@]}")
[[ -z "$SCENE" ]] || ARGS+=("$SCENE")
# Asset and shader paths have one stable root. Model paths are already absolute.
cd -- "$BUILD_DIR"
if [[ "$DEBUG" == true ]]; then
  command -v gdb >/dev/null || fail "gdb is required for --debug"
  exec gdb --args "$TARGET" "${ARGS[@]}"
fi
exec "$TARGET" "${ARGS[@]}"
