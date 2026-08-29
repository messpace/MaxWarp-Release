# MaxWarp

A lightweight sim rate changer with fuel-flow correction for MSFS 2024.
Targets the iniBuilds A330-200/300, whose engine model burns fuel at 1x
regardless of sim rate; MaxWarp corrects the burn externally via 1 Hz
SimConnect read-modify-write of the legacy `FUEL TANK <NAME> QUANTITY` simvars.
Also automatically slows back down to a desired rate during turns to enable
time-accelerated flights without having to be physically present at your PC.

## Download

Grab the latest [release](https://github.com/messpace/MaxWarp-Release/releases).

- **`MaxWarp-vX.Y.Z-win-x64-setup.exe`** — installer, with a Start Menu entry
  and an uninstaller.
- **`MaxWarp-vX.Y.Z-win-x64.zip`** — portable; unzip anywhere and run
  `MaxWarp.exe`.

## Build (Windows, MSVC x64)

Requirements: Visual Studio 2022 or newer with the C++ workload, CMake >= 3.21,
and Qt 6.10.2 (`msvc2022_64`). SimConnect needs no install — the vendored copy
in `third_party/` is the default.

```powershell
cmake -S . -B build -A x64 -DMAXWARP_QT_ROOT="C:/Qt/6.10.2/msvc2022_64"
cmake --build build --config Release
```

Point it at your own SDK instead with
`-DSIMCONNECT_SDK_ROOT="C:/MSFS 2024 SDK/SimConnect SDK"`.

## Run from a build tree

```powershell
$env:PATH = "C:\Qt\6.10.2\msvc2022_64\bin;$env:PATH"
.\build\Release\MaxWarp.exe
```

The app connects to a running MSFS 2024 automatically (retrying every 2 s) and
logs live sim rate plus tank quantities to the console while showing them in the
window. `SimConnect.dll` is copied next to the exe at build time; the Qt DLLs
resolve via `PATH` during development, and via `windeployqt` in a release build.

## License

MIT — see `LICENSE`. The bundled typefaces are OFL; their licenses ship in
`fonts/`. The vendored SimConnect files are Microsoft's and are covered by the
MSFS SDK terms, not by the MIT license above — see
`third_party/SimConnect/README.md`.

The distributed builds bundle the Qt 6 runtime, which is used under the LGPL
v3. Qt is dynamically linked and its DLLs ship unmodified beside the
executable, so it can be replaced with another build of the same Qt version;
the Qt sources are available from https://download.qt.io.
