#!/bin/sh
# Runs the test inside a throw-away muvm VM in which the patched FEX is registered as the x86 interpreter
# (the same registration scripts/vm/steam-vm.sh does). Needs scripts/vm/install-overlay.sh to have been run.
#   ./run-in-vm.sh [path/to/ptrace_inject_test]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../.." && pwd)
BIN=$(readlink -f "${1:-$HERE/ptrace_inject_test}")
OVERLAY=${FEX_OVERLAY_DIR:-$HOME/.local/share/vrchat-fex-eac/fex}/FEX
STATE=${XDG_STATE_HOME:-$HOME/.local/state}/vrchat-fex-eac; mkdir -p "$STATE"
OUT="$STATE/ptrace-test.out"; rm -f "$OUT"
cat > "$STATE/ptrace-test-setup.sh" <<WRAP
#!/bin/sh
export FEX_OVERLAY_BIN='$OVERLAY' REALISM=0
exec '$REPO/scripts/vm/guest-setup.sh'
WRAP
cat > "$STATE/ptrace-test-run.sh" <<WRAP
#!/bin/sh
cd /tmp; timeout 60 '$BIN' > '$OUT' 2>&1; echo "exit=\$?" >> '$OUT'
WRAP
chmod +x "$STATE"/ptrace-test-*.sh
rm -rf "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/krun"
timeout 120 muvm --mem 2048 -x "$STATE/ptrace-test-setup.sh" -- /bin/sh "$STATE/ptrace-test-run.sh" >/dev/null 2>&1 || true
sleep 1; cat "$OUT"
grep -q "RESULT: PASS" "$OUT"
