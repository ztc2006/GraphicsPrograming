#!/usr/bin/env bash
set -euo pipefail
SOURCE_ROOT="$1"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/project/build-linux" "$TMP/bin" "$TMP/caller"
cp "$SOURCE_ROOT/run.sh" "$TMP/project/run.sh"
cat > "$TMP/bin/cmake" <<'MOCK'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$LAUNCH_TEST_LOG"
MOCK
cat > "$TMP/project/build-linux/vulkan" <<'MOCK'
#!/usr/bin/env bash
printf 'cwd=%s\n' "$PWD" > "$LAUNCH_TEST_ARGS"
printf '%s\n' "$@" >> "$LAUNCH_TEST_ARGS"
MOCK
chmod +x "$TMP/bin/cmake" "$TMP/project/build-linux/vulkan"
export PATH="$TMP/bin:$PATH" LAUNCH_TEST_LOG="$TMP/build.log" LAUNCH_TEST_ARGS="$TMP/args.log"
touch "$TMP/caller/model with spaces.GLB"
cd "$TMP/caller"
bash "$TMP/project/run.sh" "model with spaces.GLB"
test -s "$LAUNCH_TEST_LOG"
[[ $(tail -n 1 "$LAUNCH_TEST_ARGS") == "$TMP/caller/model with spaces.GLB" ]]
: > "$LAUNCH_TEST_LOG"
bash "$TMP/project/run.sh" --no-build
test ! -s "$LAUNCH_TEST_LOG"
if bash "$TMP/project/run.sh" missing.glb; then echo 'Missing model was accepted' >&2; exit 1; fi
test ! -s "$LAUNCH_TEST_LOG"
if bash "$TMP/project/run.sh" --no-build one.glb two.glb; then exit 1; fi
: > "$LAUNCH_TEST_ARGS"
bash "$TMP/project/run.sh" --build-only
test ! -s "$LAUNCH_TEST_ARGS"
cd "$TMP/caller"
bash "$TMP/project/run.sh" --no-build --benchmark "results with spaces" --size 1920x1080 --warmup 0 --duration 2 --gpu "RTX 2060" --no-ui "model with spaces.GLB"
python3 - "$LAUNCH_TEST_ARGS" "$TMP/caller" <<'PY'
import pathlib,sys
lines=pathlib.Path(sys.argv[1]).read_text().splitlines()[1:]
assert lines == ['--benchmark',sys.argv[2]+'/results with spaces','--size','1920x1080','--warmup','0','--duration','2','--gpu','RTX 2060','--no-ui',sys.argv[2]+'/model with spaces.GLB'], lines
PY
