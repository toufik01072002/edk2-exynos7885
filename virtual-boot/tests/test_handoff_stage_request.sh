#!/bin/sh
set -eu
SCRIPT=$1
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/boot"
: > "$TMP/boot/BootShim.bin"
: > "$TMP/boot/descriptor.bin"
LOG="$TMP/calls"
FAKE="$TMP/fake-stager"
cat > "$FAKE" <<'FAKEEOF'
#!/bin/sh
set -eu
printf '%s\n' "$*" >> "$CALL_LOG"
printf 'staged' > "$3"
FAKEEOF
chmod +x "$FAKE"
mkdir -p "$TMP/tool dir"
cp "$FAKE" "$TMP/tool dir/fake stager"

# Missing input must fail closed without invoking the stager.
if BOOTSHIM_PATH="$TMP/missing.bin" DESCRIPTOR_PATH="$TMP/boot/descriptor.bin" \
   OUTPUT_PATH="$TMP/out.bin" HANDOFF_STAGE_BIN="$FAKE" CALL_LOG="$LOG" \
   sh "$SCRIPT" >"$TMP/missing.log" 2>&1; then
    echo "FAIL: missing input was accepted"; exit 1
fi
test ! -e "$LOG"

# Existing output must never be overwritten.
printf 'keep' > "$TMP/existing.bin"
if BOOTSHIM_PATH="$TMP/boot/BootShim.bin" DESCRIPTOR_PATH="$TMP/boot/descriptor.bin" \
   OUTPUT_PATH="$TMP/existing.bin" HANDOFF_STAGE_BIN="$FAKE" CALL_LOG="$LOG" \
   sh "$SCRIPT" >"$TMP/existing.log" 2>&1; then
    echo "FAIL: existing output was accepted"; exit 1
fi
test "$(cat "$TMP/existing.bin")" = keep
test ! -e "$LOG"

# Complete inputs call the stager exactly once and create the requested output.
BOOTSHIM_PATH="$TMP/boot/BootShim.bin" DESCRIPTOR_PATH="$TMP/boot/descriptor.bin" \
OUTPUT_PATH="$TMP/result.bin" HANDOFF_STAGE_BIN="$FAKE" CALL_LOG="$LOG" \
sh "$SCRIPT"
test "$(cat "$TMP/result.bin")" = staged
test "$(wc -l < "$LOG")" -eq 1
grep -Fq "$TMP/boot/BootShim.bin $TMP/boot/descriptor.bin $TMP/result.bin" "$LOG"
# A missing/non-executable stager must fail closed.
if BOOTSHIM_PATH="$TMP/boot/BootShim.bin" DESCRIPTOR_PATH="$TMP/boot/descriptor.bin" \
   OUTPUT_PATH="$TMP/no-stager.bin" HANDOFF_STAGE_BIN="$TMP/missing-stager" \
   sh "$SCRIPT" >"$TMP/no-stager.log" 2>&1; then
    echo "FAIL: missing stager was accepted"; exit 1
fi
test ! -e "$TMP/no-stager.bin"

# Executable paths containing spaces must be passed as one command.
BOOTSHIM_PATH="$TMP/boot/BootShim.bin" DESCRIPTOR_PATH="$TMP/boot/descriptor.bin" \
OUTPUT_PATH="$TMP/spaces-result.bin" HANDOFF_STAGE_BIN="$TMP/tool dir/fake stager" \
CALL_LOG="$LOG" sh "$SCRIPT"
test "$(cat "$TMP/spaces-result.bin")" = staged
test "$(wc -l < "$LOG")" -eq 2
echo "PASS: request wrapper blocks missing inputs, overwrite and missing stager; quotes executable paths"
