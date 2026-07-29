# Building

## What you need

| Tool | Version | Why |
|---|---|---|
| C++ compiler | C++20 | MSVC 19.3x+, GCC 12+, or Clang 15+ |
| CMake | ≥ 3.22 | presets, FetchContent |
| Ninja | any | the presets use it; the VS generator also works |
| Python | 3.11+ | recorder and analysis |
| Git | any | FetchContent pulls GoogleTest at configure time |

First configure needs network access: GoogleTest (and Google Benchmark, when
`LOB_BUILD_BENCH=ON`) are fetched then. Afterwards the build is offline.

---

## Windows

Visual Studio Community alone is **not** enough — the C++ workload is a separate
install, and without the Windows SDK `cl.exe` cannot find `stdio.h`.

```powershell
# Verify what you actually have.  This compiles AND RUNS a probe program,
# because "cl.exe exists" does not mean "cl.exe works".
powershell -ExecutionPolicy Bypass -File tools\check_toolchain.ps1
```

If it reports a missing SDK or a failed compile:

```
Right-click tools\install_cpp_toolchain.cmd  ->  "Run as administrator"
```

That adds, to your existing Visual Studio Community install:

- MSVC v14.5x C++ compiler and standard library
- Windows 11 SDK (the CRT headers `cl.exe` needs)
- C++ CMake tools for Windows (`cmake.exe` + `ninja.exe`)
- AddressSanitizer support

Roughly 8–10 GB. Safe to re-run. **It requires an elevation prompt — you have to
click Yes on the UAC dialog**; the install silently does nothing if the prompt is
dismissed.

Then, from a *Developer PowerShell for VS* (or any shell after running
`VsDevCmd.bat`):

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-debug --parallel
ctest --preset msvc-debug --output-on-failure
```

### Alternative: MinGW-w64

If you would rather not install the Visual Studio C++ workload:

```powershell
winget install MSYS2.MSYS2
winget install Kitware.CMake
winget install Ninja-build.Ninja
# then, in the MSYS2 UCRT64 shell:
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-gdb
```

GCC also gives you `-fsanitize=address,undefined`, which MSVC does not (MSVC has
ASan only). The `gcc-debug-sanitize` preset is the one the CI debug job uses.

---

## Linux / macOS

```bash
sudo apt-get install -y build-essential cmake ninja-build g++-13   # Debian/Ubuntu
brew install cmake ninja                                          # macOS

cmake --preset gcc-debug-sanitize
cmake --build --preset gcc-debug-sanitize --parallel
ctest --preset gcc-debug-sanitize --output-on-failure
```

---

## Presets

| Preset | Build type | Notable options |
|---|---|---|
| `msvc-debug` | Debug | `LOB_DUAL_BOOK=ON` |
| `msvc-release` | RelWithDebInfo | `LOB_BUILD_BENCH=ON` |
| `msvc-asan` | Debug | `LOB_SANITIZE=ON`, `LOB_DUAL_BOOK=ON` |
| `gcc-debug-sanitize` | Debug | ASan + UBSan, `LOB_DUAL_BOOK=ON` — the CI debug job |
| `gcc-release` | RelWithDebInfo | `LOB_BUILD_BENCH=ON` |

## Options

| Option | Default | Meaning |
|---|---|---|
| `LOB_BUILD_TESTS` | `ON` | GoogleTest suite (fetches GoogleTest) |
| `LOB_BUILD_BENCH` | `OFF` | Google Benchmark targets |
| `LOB_BUILD_APPS` | `ON` | `lob_replay`, `lob_sweep`, `lob_calibrate`, `lob_convert` |
| `LOB_SANITIZE` | `OFF` | `-fsanitize=address,undefined` (MSVC: ASan only) |
| `LOB_WERROR` | `ON` | warnings are errors |
| `LOB_STRICT_WARNINGS` | `OFF` | adds `-Wconversion`, `-Wold-style-cast`, … on GCC/Clang |
| `LOB_DUAL_BOOK` | `OFF` | compile the dense-vs-map cross-check into the book core |

`LOB_STRICT_WARNINGS` is off by default deliberately: those flags are not part
of the CLAUDE.md contract, and turning them on together with `-Werror` is a
promise that should be made only after it has been verified on the toolchain in
question, not assumed.

---

## Never benchmark a checked build

`LOB_DUAL_BOOK` runs every book operation twice and compares; `LOB_SANITIZE`
costs several times more again. `bench/bench_replay.cpp` includes
`BM_ReplayDualBook` specifically so the size of that distortion is visible.
Benchmark with `msvc-release` / `gcc-release` only, on an otherwise idle machine,
and record the hardware in `bench/RESULTS.md`.

---

## Python

Two independent environments; neither is needed to build the C++.

```bash
python -m pip install -r recorder/requirements.txt   # websockets
python -m pip install -r analysis/requirements.txt   # pandas, numpy, matplotlib
```

On Windows, if `python` on `PATH` is a broken or partial install, call the
working interpreter by full path:

```powershell
& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" recorder\recorder.py --config recorder\symbols.yaml
```

`tools\check_toolchain.ps1` reports which interpreters it found working.

---

## Troubleshooting

**`fatal error C1083: Cannot open include file: 'stdio.h'`**
The Windows SDK is missing. Run `tools\install_cpp_toolchain.cmd` as
administrator and accept the UAC prompt.

**`CMake Error: Could not find CMAKE_CXX_COMPILER`**
Configure from a Developer PowerShell, or run
`"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64`
first.

**FetchContent fails at configure**
First configure needs network access to fetch GoogleTest. To build without it,
pass `-DLOB_BUILD_TESTS=OFF` — the libraries and apps have no external
dependencies at all.

**Tests pass but the golden test says "bootstrapped"**
Expected on a fresh clone: `tests/fixtures/` has no committed expectations yet,
so the golden test writes them and skips instead of passing vacuously. Review
the generated files, commit them, and it becomes a hard gate.
