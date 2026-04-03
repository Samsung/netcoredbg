#!/usr/bin/env bash
# Test breakpoints in netcoredbg CLI mode.
# Strategy: function-break on main to stop after module load,
# then set line breakpoints (which now resolve against loaded modules).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DLL="$SCRIPT_DIR/FSharpCeBreakpointRepro/bin/Debug/net8.0/FSharpCeBreakpointRepro.dll"
NETCOREDBG="${NETCOREDBG:-$HOME/.local/bin/netcoredbg/netcoredbg}"

if [[ ! -f "$DLL" ]]; then
    echo "Build first: dotnet build -c Debug"
    exit 1
fi

echo "=== netcoredbg CLI mode breakpoint test ==="
echo "Strategy: break on main, run, then set line breakpoints after module load."
echo ""

# Use a FIFO for controlled input
FIFO=$(mktemp -u)
mkfifo "$FIFO"

"$NETCOREDBG" --interpreter=cli -- dotnet exec "$DLL" < "$FIFO" 2>&1 &
DBG_PID=$!
exec 3>"$FIFO"

send() {
    echo ">>> $1" >&2
    echo "$1" >&3
    sleep 1
}

sleep 1

# Set function breakpoint on main, then run
send "break FSharpCeBreakpointRepro.main"
send "run"
sleep 3

# Now modules are loaded — set line breakpoints
send "break Program.fs:8"
send "break Program.fs:17"

# Continue — should hit line 8
send "continue"
sleep 1

# Continue — should hit line 17 (CE block)
send "continue"
sleep 1

# Let it finish
send "continue"
sleep 2

send "quit"
sleep 0.5

exec 3>&-
rm -f "$FIFO"
wait $DBG_PID 2>/dev/null || true
echo ""
echo "=== Done ==="
