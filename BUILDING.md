# Building from source

Ready-made builds are on the Releases page. Follow this guide only if you want to
build the game yourself.

You don't need the ROM to build. You do need it to play, because the game reads its
assets from your own copy of the ROM when it starts.

## What you need

- Git
- CMake 3.20 or newer
- Make
- A C11 and C++20 compiler (GCC or Clang)
- Python 3
- SDL2 and zlib development files
- OpenGL

Ragdoll physics uses [Jolt Physics](https://github.com/jrouwe/JoltPhysics). You don't
have to install Jolt. If CMake can't find a local copy, it downloads Jolt v5.6.0
with git during the configure step, so that first configure needs an internet
connection.

Visual Studio (MSVC) is not supported. On Windows, use MSYS2.

## Get the source

```sh
git clone --recursive https://github.com/Chesluk-Industries/Perfect-Dark-CE.git
cd Perfect-Dark-CE
```

If you already cloned without `--recursive`, run this:

```sh
git submodule update --init --recursive
```

## Windows (MSYS2)

1. Install [MSYS2](https://www.msys2.org/).
2. Open the **MSYS2 MINGW64** shell. Don't use the plain "MSYS" shell.
3. Install the packages:

   ```sh
   pacman -S --needed git make mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
     mingw-w64-x86_64-SDL2 mingw-w64-x86_64-zlib mingw-w64-x86_64-python3
   ```

4. Build:

   ```sh
   cmake -G "Unix Makefiles" -B build .
   cmake --build build -j$(nproc)
   ```

   This produces `build/pd.x86_64.exe`.

5. Put these DLLs next to the exe. They are in `/mingw64/bin`:
   - `SDL2.dll`
   - `zlib1.dll`
   - `libwinpthread-1.dll`
   - `libgcc_s_seh-1.dll`

### Helper script

`build_windows.sh` builds the game and puts the exe, the DLLs and the `data` folder
together in one folder. Run it from the MINGW64 shell:

```sh
./build_windows.sh                        # incremental build
./build_windows.sh clean                  # wipe the build folder first
PD_OUT_DIR=../my_build ./build_windows.sh # choose the output folder
```

### 32-bit Windows

Use the **MSYS2 MINGW32** shell. Install the same packages with the
`mingw-w64-i686-` prefix in place of `mingw-w64-x86_64-`. Then build with the same
`cmake` commands. You get `pd.i686.exe`.

The DLLs are in `/mingw32/bin`. Copy `libgcc_s_dw2-1.dll` instead of
`libgcc_s_seh-1.dll`.

## Linux

Install the packages for your distribution.

Debian or Ubuntu:

```sh
sudo apt install git cmake make gcc g++ python3 libsdl2-dev zlib1g-dev libgl1-mesa-dev
```

Fedora:

```sh
sudo dnf install git cmake make gcc gcc-c++ python3 SDL2-devel zlib-devel mesa-libGL-devel
```

Arch:

```sh
sudo pacman -S --needed git cmake make gcc python sdl2 zlib mesa
```

Then build:

```sh
cmake -G "Unix Makefiles" -B build .
cmake --build build -j$(nproc)
```

This produces `build/pd.x86_64`.

To build a 32-bit binary on a 64-bit system, install the 32-bit (multilib)
compilers, SDL2 and zlib. Then add these flags when you configure:

```sh
cmake -G "Unix Makefiles" -B build -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 .
```

## macOS

1. Install the Xcode command line tools: `xcode-select --install`
2. Install CMake, Python 3 and zlib, for example with Homebrew:
   `brew install cmake python zlib`
3. Download `SDL2-2.30.9.dmg` from [libsdl.org](https://libsdl.org/release/). Copy
   `SDL2.framework` from it into `/Library/Frameworks`.
4. Build:

   ```sh
   cmake -G "Unix Makefiles" -B build -DCMAKE_OSX_ARCHITECTURES=arm64 .
   cmake --build build -j$(sysctl -n hw.ncpu)
   ```

   On an Intel Mac, use `-DCMAKE_OSX_ARCHITECTURES=x86_64`.

## Build options

Pass these to the first `cmake` command as `-DNAME=value`.

| Option | Default | What it does |
|---|---|---|
| `ROMID` | `ntsc-final` | The ROM the build is for: `ntsc-final`, `pal-final` or `jpn-final` |
| `PD_PHYSICS` | `ON` | Ragdoll physics. `OFF` builds without Jolt |
| `JOLT_DIR` | *(none)* | Path to a local JoltPhysics source tree. If it isn't set, CMake downloads Jolt |
| `PD_JOLT_TAG` | `v5.6.0` | The Jolt version to download |
| `PD_CLIPCHECK` | `ON` | Clipping checks against the level's geometry |
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` | Use `Release` for a build without debug info |

For example, this builds for the PAL ROM:

```sh
cmake -G "Unix Makefiles" -B build-pal -DROMID=pal-final .
cmake --build build-pal -j$(nproc)
```

Each ROM version needs its own build folder.

## Setting up the game folder

Put these together in one folder:

```
pd.x86_64.exe            (or pd.x86_64 on Linux/macOS)
SDL2.dll, ...            (Windows only, see above)
data/
  pd.ntsc-final.z64      your ROM (pd.pal-final.z64 / pd.jpn-final.z64 for those builds)
  sounds/                copied from port/data/sounds
```

1. Copy everything inside `port/data/` into `data/`. It holds extra sounds the
   ragdolls use.
2. Put your ROM in `data/` with the name shown above. It must be a `.z64`
   (big-endian) ROM.

`PLAYER_GUIDE.txt` lists the settings and controls.

## Troubleshooting

- **CMake fails while downloading Jolt.**
  - Check that git is installed and that you're online.
  - If you can't download, clone JoltPhysics yourself and pass `-DJOLT_DIR=<path>`.
  - Or build without ragdolls: `-DPD_PHYSICS=OFF`.
- **"Please run this from the MSYS2 MINGW64 shell".** `build_windows.sh` only works in
  the MINGW64 shell.
- **A DLL is missing when you start the exe.** Copy the DLLs listed for your shell
  from `/mingw64/bin` or `/mingw32/bin`.
- **The game says it can't find the ROM.** Check that the file name and folder match
  the layout above. Also check that the ROM version matches the build's `ROMID`.
- **The build gets confused after you change options.** Delete the build folder and
  configure again.
