#!/usr/bin/env bash
# Builds a shareable release of the ragdoll mod and puts it together under
# ../../_release/ (two folders up from the port).
#
# Run from the MSYS2 MINGW64 shell:
#   cd "<the port folder>"
#   ./make_release.sh                 # folder name: PerfectDark-Ragdolls-<date>
#   ./make_release.sh MyName          # folder name: MyName-<date>
#
# It builds into build_release/ (your normal build_win/ and pd_pc_build/ are
# not touched) and makes:
#   _release/<name>-<date>/game/      the game, ready to zip and share (no ROM, no pd.ini)
#   _release/<name>-<date>/source/    every new or modified source file, same layout as the port,
#                                     what each change is (fix, non-vanilla, ragdoll) and the
#                                     split patches from ../../_split
#   _release/<name>-<date>-symbols/   the unstripped exe, to read testers' crash logs (keep, don't share)
set -e
cd "$(dirname "$0")"

if [ "$MSYSTEM" != "MINGW64" ]; then
  echo "Please run this from the MSYS2 MINGW64 shell (current: ${MSYSTEM:-unknown})."
  exit 1
fi

NAME="${1:-PerfectDark-Ragdolls}"
STAMP="$(date +%Y-%m-%d_%H%M)"
BUILD_DIR=build_release
REL_ROOT="../../_release"
OUT="$REL_ROOT/$NAME-$STAMP"
SYM="$REL_ROOT/$NAME-$STAMP-symbols"
VANILLA="../../_reference/port_src_vanilla"
PCBUILD="../../pd_pc_build"

# ---- build ----
rm -rf "$BUILD_DIR" # always a clean build (no stale cache or objects)
cmake -G"Unix Makefiles" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DPD_PHYSICS=ON -DPD_CLIPCHECK=ON .
cmake --build "$BUILD_DIR" -j"$(nproc)" -- -O

EXE="$BUILD_DIR/pd.x86_64.exe"

# ---- game folder ----
mkdir -p "$OUT/game/data" "$SYM"
cp "$EXE" "$SYM/pd.x86_64.exe"
cp "$EXE" "$OUT/game/pd.x86_64.exe"
strip --strip-debug "$OUT/game/pd.x86_64.exe"
cp /mingw64/bin/{SDL2.dll,zlib1.dll,libwinpthread-1.dll,libgcc_s_seh-1.dll} "$OUT/game/"

if [ -f "$PCBUILD/gamecontrollerdb.txt" ]; then
  cp "$PCBUILD/gamecontrollerdb.txt" "$OUT/game/"
fi

# The game's extra data (data/sounds/: the ragdolls' sounds)
cp -r port/data/. "$OUT/game/data/"

cat > "$OUT/game/run_with_log.bat" <<'EOF'
@echo off
rem Starts the game with logging: pd.log is written next to the exe
cd /d "%~dp0"
start "" pd.x86_64.exe --log %*
EOF

cat > "$OUT/game/data/PUT_YOUR_ROM_HERE.txt" <<'EOF'
Put your own Perfect Dark ROM in this folder, named:

    pd.ntsc-final.z64

(NTSC 1.1 / "final", md5 e03b088b6ac9e0080440efed07c1e40f.)
The ROM is not included and can't be shared.
EOF

cat > "$OUT/game/README.txt" <<EOF
Perfect Dark PC port - ragdoll physics build ($STAMP)

Based on the Perfect Dark PC port by fgsfdsfgs and contributors.

SETUP
  1. Put your Perfect Dark ROM in data\ as pd.ntsc-final.z64 (see data\PUT_YOUR_ROM_HERE.txt).
  2. Run pd.x86_64.exe. Use run_with_log.bat instead to write pd.log, useful for bug reports.

WHAT'S NEW
  - Ragdoll physics (Jolt Physics): bodies fall, tumble, get pushed by shots,
    blasts and characters, and can be grabbed (hold Use).
  - Death animations blend into physics; stiffness, reflexes, bracing,
    clutching wounds, headshot limpness.
  - Dismemberment from strong weapons, explosions and full shotgun blasts;
    blood spray, blood that wraps round corners.
  - Immersive death camera: you see through your own body's eyes as it falls,
    and your gun drops from your hands.
  - Accurate collision: bodies collide with the level as it's drawn, not only
    the game's simpler collision (for the player too, as an experimental
    option under Physics > Debug).
  - Rendering fixes: decal clamping limited to bullet holes, blood and scorch
    marks; forced-opaque rock textures; flare occlusion; unlimited decals option.
  - Smooth play above 60 FPS, full effects in co-op, 5.1 surround output.
  See CHANGELOG.txt for the changes since the previous build.

SETTINGS AND CONTROLS
  Everything this build adds (every option, its default and what it does,
  the controls, the Deadly Punch cheat and the pd.ini-only settings) is in
  PLAYER_GUIDE.txt.
  For play as close to the original as possible, turn Ragdolls off (see
  PLAYER_GUIDE.txt, section 12).

CRASHES
  If the game crashes, a pd.crash-<date>.log is written next to the exe.
  Please send it along with pd.log (from run_with_log.bat).
EOF

cp PLAYER_GUIDE.txt "$OUT/game/PLAYER_GUIDE.txt"

if [ -f CHANGELOG.txt ]; then
  cp CHANGELOG.txt "$OUT/game/CHANGELOG.txt"
fi

# Windows line endings for the text files players open
for f in "$OUT/game/run_with_log.bat" "$OUT/game/README.txt" "$OUT/game/PLAYER_GUIDE.txt" "$OUT/game/data/PUT_YOUR_ROM_HERE.txt" "$OUT/game/CHANGELOG.txt"; do
  [ -f "$f" ] && sed -i 's/\r*$/\r/' "$f"
done

# ---- source folder ----
mkdir -p "$OUT/source"
LIST="$OUT/source/CHANGED_FILES.txt"
: > "$LIST.tmp"

# Every file under a folder (src/assets left out) as "path<TAB>md5", sorted.
# Compared with md5sum (always there), not diff (MSYS2 doesn't always have it,
# and translates its messages).
tree_sums() {
  (cd "$1" && find . -name assets -prune -o -type f -print0 | xargs -0 -r md5sum \
    | sed -E 's/^([0-9a-f]+) [ *]\.\/(.*)$/\2\t\1/' | LC_ALL=C sort)
}

# Game code: everything that differs from the vanilla reference (or is new)
if [ -d "$VANILLA" ]; then
  tree_sums "$VANILLA" > "$LIST.van"
  tree_sums src > "$LIST.src"
  awk -F'\t' 'NR == FNR { v[$1] = $2; next }
    !($1 in v) { print "new       src/" $1; next }
    v[$1] != $2 { print "modified  src/" $1 }' "$LIST.van" "$LIST.src" >> "$LIST.tmp"
  rm -f "$LIST.van" "$LIST.src"
  if ! grep -q " src/" "$LIST.tmp"; then
    echo "WARNING: no game code differs from $VANILLA: check that it's the vanilla port's src folder"
  fi
else
  echo "WARNING: no vanilla reference at $VANILLA; game code changes found by MOD tags only"
  grep -rlE "MOD:" src | sed 's/^/modified  /' >> "$LIST.tmp"
fi

# Port code and build files: the files carrying MOD: hooks
grep -rlE "MOD:" port CMakeLists.txt | sort | while read -r f; do
  case "$f" in
    port/src/ragdoll.c|port/src/clipcheck.c|port/src/clipcheck/*|port/src/bloodwrap.c|port/include/ragdoll.h|port/include/clipcheck.h)
      echo "new       $f" >> "$LIST.tmp" ;;
    *)
      echo "modified  $f" >> "$LIST.tmp" ;;
  esac
done

# The physics module and the ragdoll module's files (all new)
find physics port/src/ragdoll port/data -type f | sed 's/^/new       /' >> "$LIST.tmp"

# The scripts, the guides and the category table
for f in make_release.sh build_windows.sh tools/crash_symbols.sh PLAYER_GUIDE.txt SOURCE_GUIDE.txt tools/mod_categories.txt; do
  echo "new       $f" >> "$LIST.tmp"
done

sort -b -k2,2 -u "$LIST.tmp" > "$LIST.files"
rm -f "$LIST.tmp"

awk '{print $2}' "$LIST.files" | while read -r f; do
  mkdir -p "$OUT/source/$(dirname "$f")"
  cp "$f" "$OUT/source/$f"
done

# ---- what is what: each file's kinds of change (tools/mod_categories.txt) ----
CATS=tools/mod_categories.txt

# The MOD tags in a file ("MOD: <tag> - ...", "MOD: <tag> (...)", "MOD: <tag>.")
mod_tags() {
  grep -ohE 'MOD: [^*/"]*' "$1" 2>/dev/null \
    | sed -E 's/^MOD: //; s/( - |\(|\. |\.$|\)|:|;|,).*//; s/ +$//' | sort -u
}

# A tag's category, or ? if the table doesn't have it
tag_cat() {
  awk -F'|' -v t="$1" '!/^#/ && $1 == t { print $2; found = 1; exit } END { if (!found) print "?" }' "$CATS"
}

# A file's kinds of change: F, N, R, or - (build scripts and guides)
file_cats() {
  local f="$1" c="" t
  case "$f" in
    physics/*|port/data/*|port/src/ragdoll*|port/src/clipcheck*|port/src/bloodwrap.c|port/include/ragdoll.h|port/include/clipcheck.h|make_release.sh|PLAYER_GUIDE.txt)
      echo "R"; return ;;
    SOURCE_GUIDE.txt|tools/mod_categories.txt|build_windows.sh)
      echo "-"; return ;;
  esac
  while IFS= read -r t; do
    if [ -n "$t" ]; then c="$c $(tag_cat "$t")"; fi
  done < <(mod_tags "$f")
  if grep -qE 'PD_PHYSICS|PD_CLIPCHECK' "$f"; then
    c="$c R"
  fi
  c="$(echo $c | tr ' ' '\n' | sort -u | tr '\n' ' ' | sed 's/ *$//')"
  echo "${c:--}"
}

{
  echo "# Every new or modified source file of this build, and the kinds of change in it:"
  echo "#   F  fix: the port behaves as the original game does (mostly high frame rates)"
  echo "#   N  non-vanilla: a change from the original outside the ragdoll module"
  echo "#   R  ragdoll / physics (the ragdoll module, accurate collision)"
  echo "#   -  build scripts and guides"
  echo "# See SOURCE_GUIDE.txt, and MOD_INDEX.txt for each change and its files."
  echo "#"
  while read -r kind f; do
    printf "%-9s %-50s %s\n" "$kind" "$f" "$(file_cats "$f")"
  done < "$LIST.files"
} > "$LIST"
rm -f "$LIST.files"

# MOD_INDEX.txt: each tag, its category, what it is and the files it's in
INDEX="$OUT/source/MOD_INDEX.txt"
{
  echo "Every change in this build's source, by kind. Each one is marked in the code"
  echo "with a comment \"MOD: <tag>\". F = fix (as the original), N = non-vanilla,"
  echo "R = ragdoll / physics; see SOURCE_GUIDE.txt."
  for cat in F N R; do
    echo
    case $cat in
      F) echo "=== F: FIXES (the port behaves as the original) ===" ;;
      N) echo "=== N: NON-VANILLA (outside the ragdoll module) ===" ;;
      R) echo "=== R: RAGDOLL / PHYSICS ===" ;;
    esac
    grep -v '^#' "$CATS" | awk -F'|' -v c=$cat '$2 == c' | while IFS='|' read -r tag c desc; do
      files="$(cd "$OUT/source" && grep -rlF "MOD: $tag" --include='*.c' --include='*.h' --include='*.cpp' --include='*.sh' --include='CMakeLists.txt' . 2>/dev/null | sed 's|^\./||' | sort | tr '\n' ' ')"
      [ -n "$files" ] || continue
      echo
      echo "[$cat] $tag"
      echo "    $desc"
      echo "$files" | fold -s -w 74 | sed 's/ *$//; s/^/    /'
    done
  done
  echo
  echo "Ragdoll / physics also: everything inside #ifdef PD_PHYSICS or"
  echo "#ifdef PD_CLIPCHECK, and the files physics/, port/src/ragdoll*,"
  echo "port/src/clipcheck*, port/src/bloodwrap.c, port/include/ragdoll.h,"
  echo "port/include/clipcheck.h."
} > "$INDEX"

# Tags the table doesn't know (add them to tools/mod_categories.txt)
UNKNOWN="$(grep -v '^#' "$LIST" | awk '$3 != "R" && $3 != "-" {print $2}' | while read -r f; do mod_tags "$f"; done \
  | sort -u | while IFS= read -r t; do if [ -n "$t" ] && [ "$(tag_cat "$t")" = "?" ]; then echo "  $t"; fi; done)"
if [ -n "$UNKNOWN" ]; then
  echo "WARNING: MOD tags missing from $CATS (marked ? in CHANGED_FILES.txt):"
  echo "$UNKNOWN"
fi

# ---- the split: the fixes on their own, then the ragdoll module ----
SPLIT="../../_split"
if [ -f "$SPLIT/fixes.patch" ] && [ -f "$SPLIT/ragdoll.patch" ] && [ -d "$SPLIT/perfect_dark-ragdoll" ]; then
  # Only if the split was made from this very source
  STALE=""
  for d in src port physics include; do
    if [ "$(tree_sums "$SPLIT/perfect_dark-ragdoll/$d")" != "$(tree_sums "$d")" ]; then
      STALE="$STALE $d/"
    fi
  done
  if [ "$(md5sum < "$SPLIT/perfect_dark-ragdoll/CMakeLists.txt")" != "$(md5sum < CMakeLists.txt)" ]; then
    STALE="$STALE CMakeLists.txt"
  fi
  if [ -z "$STALE" ]; then
    mkdir -p "$OUT/source/patches"
    cp "$SPLIT/fixes.patch" "$SPLIT/ragdoll.patch" "$OUT/source/patches/"
    if [ -d "$SPLIT/ragdoll_files" ]; then
      cp -r "$SPLIT/ragdoll_files" "$OUT/source/patches/" # binary files a patch can't carry
    fi
    if [ -f "$SPLIT/README.txt" ]; then
      cp "$SPLIT/README.txt" "$OUT/source/patches/README.txt"
    fi
  else
    echo "WARNING: $SPLIT doesn't match this source (differs in:$STALE): the patches were left out."
  fi
else
  echo "WARNING: no split in $SPLIT: the patches were left out."
fi

cat > "$OUT/source/README.txt" <<'EOF'
New and modified source files of the ragdoll physics build, in the same
layout as the Perfect Dark PC port (copy them over a port checkout).

  SOURCE_GUIDE.txt    what is a fix, what is non-vanilla, what is ragdoll /
                      physics, and how to tell them apart in the code
  CHANGED_FILES.txt   each file, new or modified, and the kinds of change in it
  MOD_INDEX.txt       each change ("MOD: <tag>" in the code) and its files
  patches/            fixes.patch (the fixes and the non-vanilla changes,
                      without the ragdoll module) and ragdoll.patch (the
                      rest, with ragdoll_files/: its sound files); see
                      patches/README.txt

Building needs Jolt Physics: a local copy is used if there is one (CMake
option JOLT_DIR; looked for in ../../_sources/JoltPhysics-master, then
../../JoltPhysics-master), otherwise CMake downloads Jolt v5.6.0 with git.
The fixes alone (patches/fixes.patch) build without it.
EOF

# Windows line endings for the text files people open
for f in README.txt SOURCE_GUIDE.txt CHANGED_FILES.txt MOD_INDEX.txt patches/README.txt; do
  if [ -f "$OUT/source/$f" ]; then
    sed -i 's/\r*$/\r/' "$OUT/source/$f"
  fi
done

echo
echo "Game:    $(cd "$OUT/game" && pwd)"
echo "Source:  $(cd "$OUT/source" && pwd) ($(wc -l < "$LIST") files)"
echo "Symbols: $(cd "$SYM" && pwd)  (keep this one private: it reads testers' crash logs)"
