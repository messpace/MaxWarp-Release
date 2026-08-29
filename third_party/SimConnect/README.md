# SimConnect SDK

Three files from the **Microsoft Flight Simulator 2024 SDK**, vendored here so
this repository builds on a machine with no SDK installed — which is every
GitHub Actions runner.

    include/SimConnect.h
    lib/SimConnect.lib
    lib/SimConnect.dll

They are Microsoft's, not MaxWarp's, and the MIT license at the root of this
repository does not cover them. They are redistributed under the MSFS SDK terms
that permit shipping the SimConnect client library alongside an add-on. If you
have the SDK installed, point the build at your own copy instead:

    cmake -S . -B build -A x64 -DSIMCONNECT_SDK_ROOT="C:/MSFS 2024 SDK/SimConnect SDK"

`SimConnect.dll` is copied next to `MaxWarp.exe` at build time and ships in the
release archive; the app cannot start without it.
