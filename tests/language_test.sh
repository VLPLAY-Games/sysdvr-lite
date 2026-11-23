#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=../tools/language.sh
. "$HERE/tools/language.sh"
TEST_DIR="$(mktemp -d)"
trap 'rm -rf "$TEST_DIR"' EXIT
file="$TEST_DIR/options"
[ "$(sysdvr_get_language "$file")" = en ]
printf '# Keep this comment\nSYSDVR_OPTS="--decoder v4l2 --drm --no-notice"\nOTHER_SETTING=yes\n' > "$file"
cp "$file" "$TEST_DIR/expected"
chmod 640 "$file"
sysdvr_set_language "$file" ru
[ "$(sysdvr_get_language "$file")" = ru ]
[ "$(stat -c %a "$file")" = 640 ]
sysdvr_set_language "$file" en
[ "$(sysdvr_get_language "$file")" = en ]
[ "$(grep -c '^SYSDVR_LANGUAGE=' "$file")" = 1 ]
sed '/^SYSDVR_LANGUAGE=/d' "$file" > "$TEST_DIR/actual"
cmp "$TEST_DIR/expected" "$TEST_DIR/actual"
cp "$file" "$TEST_DIR/before-invalid"
if sysdvr_set_language "$file" invalid 2>/dev/null; then exit 1; fi
cmp "$file" "$TEST_DIR/before-invalid"
printf 'SYSDVR_LANGUAGE="ru"\n' > "$file"
[ "$(sysdvr_get_language "$file")" = ru ]
printf "SYSDVR_LANGUAGE='en'\n" > "$file"
[ "$(sysdvr_get_language "$file")" = en ]
echo 'Language configuration tests passed'

# Exercise the real language-only branch against fixture paths and stubbed system commands.
mkdir -p "$TEST_DIR/tools" "$TEST_DIR/bin" "$TEST_DIR/units"
cp "$HERE/tools/language.sh" "$TEST_DIR/tools/language.sh"
touch "$TEST_DIR/units/sysdvr-lite.service"
sed "s|/etc/systemd/system/|$TEST_DIR/units/|g; s|/etc/default/sysdvr-lite|$TEST_DIR/options|g" "$HERE/update.sh" > "$TEST_DIR/update.sh"
cp "$TEST_DIR/expected" "$file"
export COMMAND_LOG="$TEST_DIR/commands"
cat > "$TEST_DIR/bin/id" <<'STUB'
#!/usr/bin/env bash
printf '0\n'
STUB
cat > "$TEST_DIR/bin/findmnt" <<'STUB'
#!/usr/bin/env bash
printf 'ext4\n'
STUB
cat > "$TEST_DIR/bin/systemctl" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$COMMAND_LOG"
STUB
cat > "$TEST_DIR/bin/sleep" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB
for command in git cmake apt-get; do
    printf '#!/usr/bin/env bash\necho "Unexpected command: %s" >&2\nexit 97\n' "$command" > "$TEST_DIR/bin/$command"
done
chmod +x "$TEST_DIR/bin/"*
PATH="$TEST_DIR/bin:$PATH" bash "$TEST_DIR/update.sh" --language ru
[ "$(sysdvr_get_language "$file")" = ru ]
PATH="$TEST_DIR/bin:$PATH" bash "$TEST_DIR/update.sh" --language en
[ "$(sysdvr_get_language "$file")" = en ]
sed '/^SYSDVR_LANGUAGE=/d' "$file" > "$TEST_DIR/actual"
cmp "$TEST_DIR/expected" "$TEST_DIR/actual"
[ "$(grep -c '^restart sysdvr-lite.service$' "$COMMAND_LOG")" = 2 ]
cp "$file" "$TEST_DIR/before-invalid"
if PATH="$TEST_DIR/bin:$PATH" bash "$TEST_DIR/update.sh" --language invalid 2>/dev/null; then exit 1; fi
if PATH="$TEST_DIR/bin:$PATH" bash "$TEST_DIR/update.sh" --language ru --build 2>/dev/null; then exit 1; fi
cmp "$file" "$TEST_DIR/before-invalid"
echo 'Language-only update tests passed (no checkout, downloads or builds)'
