#!/usr/bin/env bash
# MOD: crash logs
# Turns the addresses in a crash log into function names and source lines.
# Run from the MSYS2 MINGW64 shell, with the SAME exe that crashed:
#   ./tools/crash_symbols.sh                       # ../../pd_pc_build/pd.crash.log
#   ./tools/crash_symbols.sh path/to/pd.crash-20261001-120000.log [path/to/pd.x86_64.exe]
cd "$(dirname "$0")/.."
LOG="${1:-../../pd_pc_build/pd.crash.log}"
EXE="${2:-../../pd_pc_build/pd.x86_64.exe}"

if [ ! -f "$LOG" ]; then echo "No crash log at $LOG"; exit 1; fi
if [ ! -f "$EXE" ]; then echo "No exe at $EXE"; exit 1; fi

ADDRS=$(grep -A1 "TO GET FUNCTION NAMES" "$LOG" | tail -n1 | grep -oE "0x[0-9a-fA-F]+")
if [ -z "$ADDRS" ]; then echo "No addresses for the main exe in $LOG"; exit 1; fi

echo "Backtrace of $LOG:"
i=0
for a in $ADDRS; do
  printf "#%02d %s  " "$i" "$a"
  addr2line -f -C -i -p -e "$EXE" "$a"
  i=$((i+1))
done
