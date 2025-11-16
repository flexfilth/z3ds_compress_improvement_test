# Windows Release Artifacts

This folder is reserved for the ready-to-run Windows builds of the Z3DS compressor tools.

To populate it:

1. Configure CMake with a Windows toolchain (MSVC, MinGW, or clang-cl) and build both `z3ds_compressor` and `z3ds_gui`.
2. Copy the resulting `.exe` files plus any required DLLs into this directory.
3. Optionally zip the folder before distribution.

> **Note:** The automated build environment available for this change does not have access to a Windows-capable toolchain (attempts to install one fail because the proxy blocks both `apt` and HTTPS downloads). Once you run the above steps on a Windows or MinGW-equipped machine, commit the produced executables here so other testers can consume them without additional setup.
