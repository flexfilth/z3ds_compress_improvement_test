# Z3DS Compressor

Simple cross-platform CLI tool to compress decrypted 3DS files (`.cci`, `.cxi`, `.cia`, `.3dsx`) into the seekable Z3DS
format that Azahar and other emulators can open directly. A Windows-native GUI is also included for anyone who prefers a
point-and-click workflow instead of the command line.

It can also restore `.zcia`, `.zcci`, `.zcxi`, and `.z3dsx` containers back to their original ROM formats, matching the
workflow described in [Azahar PR #1208](https://github.com/azahar-emu/azahar/pull/1208).

## Building

Thanks to the bundled Zstandard dependency, the project now builds the same way on Windows, Linux, and macOS without
shipping helper batch files. On Windows you can build both the CLI and the GUI from the same Visual Studio solution.

### Linux / macOS (Ninja example)

```
cmake --preset ninja-release
cmake --build --preset ninja-release
./build/z3ds_compressor rom.cia
```

### Windows (Visual Studio 2022 example)

```
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release
./build-msvc/Release/z3ds_compressor.exe rom.cia
./build-msvc/Release/z3ds_gui.exe
```

The Visual Studio preset downloads and links a static copy of Zstandard automatically, so there is no requirement for
MSYS/MinGW or external batch wrappers. Set `Z3DS_USE_SYSTEM_ZSTD=ON` if you prefer using a system-provided library on
Linux. The GUI target is Windows-only; on other platforms `Z3DS_ENABLE_GUI` is disabled automatically, but the CLI still
builds normally.


`z3ds_gui.exe` is a lightweight Win32 front-end that reuses the same compression engine as the CLI:

* Browse for a single ROM file or switch to *Batch (directory)* mode to compress entire folders.
* Toggle recursion, automatic source deletion, and decompression with checkboxes.
* Override frame size, compression level, and worker thread counts directly from text boxes.
* Watch a native progress bar and append-only log update in real time while the background worker runs.

The GUI intentionally mirrors the CLI options, so you can test settings visually and then move the same values into your
scripts if needed.

## Usage

Once the binary is built (or downloaded), the CLI can work on single ROMs or entire directories without relying on any
auxiliary batch files. The GUI provides the same functionality if you prefer a graphical workflow on Windows.

### Single file

```
z3ds_compressor game.cia
z3ds_compressor game.cci custom_name.zcci --level 19
```

### Batch mode

```
z3ds_compressor /storage/3ds/roms --batch
z3ds_compressor D:\ROMs --batch --delete-source --no-recursive
```

Batch mode scans the provided directory for supported extensions, optionally recursing into subfolders, and compresses
every ROM it finds.

### Command line options

```
Usage: z3ds_compressor <input> [output_file] [options]

Options:
  --batch              Treat input as a directory and process all supported files
  --no-recursive       Don't recurse into subdirectories while batching
  --delete-source      Delete the source file after successful compression
  --frame-size SIZE    Override the compression frame size in bytes (default: auto)
                       Defaults to Azahar's recommended sizes when not specified.
  --level N            Zstandard compression level (default: 15)
  --threads N          Number of Zstandard worker threads (default: CPU cores)
  --decompress         Treat the input as a .zcia/.zcci/.zcxi/.z3dsx file and restore the original
```

### Linux one-liner (legacy)

If you still prefer shell pipelines, you can mimic the classic behaviour:

```
find /path/to/roms -type f -name '*.cci' -o -name '*.cia' -print0 | \
  xargs -0 -I{} z3ds_compressor {} --level 15
```

However, the built-in `--batch` option is portable and works the same on Windows, Linux, and macOS.

### Decompression examples

```
z3ds_compressor game_backup.zcia --decompress
z3ds_compressor packed.zcci original.cci --decompress
```

When no output filename is provided, the tool automatically swaps the `.zcia`/`.zcci`/`.zcxi`/`.z3dsx` extension back to
its original counterpart.

