# SIV — SAR Image Viewer

A fast, interactive viewer for SICD and SIDD NITF SAR imagery built with Qt5.

![SIV screenshot](Screenshot%202026-03-16%20130350.png)

## Features

- **SICD & SIDD support** — opens NGA-standard NITF files for both complex (SICD) and derived (SIDD) SAR products
- **dB magnitude display** — renders complex data as 20·log₁₀(|z|) for perceptually correct contrast
- **Interactive histogram** — drag the lo/hi handles on the log-scale bar chart to adjust stretch without reloading
- **Tiled rendering** — image is split into tiles that load in parallel on all available CPU threads; tile size is matched to the NITF file's internal block size to minimise seeks
- **Smooth zoom & pan** — mouse-wheel zoom with kinetic pan; overview is shown instantly while full-resolution tiles stream in
- **XML metadata** — embedded SICD/SIDD XML shown in a collapsible tree alongside the image
- **Self-contained install** — the install step bundles all Qt and system `.so` dependencies; the result runs on any compatible Linux without a Qt installation

## Building

### Requirements

| Dependency | Minimum version | Notes |
|---|---|---|
| CMake | 3.17 | |
| C++ compiler | C++17 | GCC 9+ or Clang 10+ |
| Qt5 | 5.12 | Widgets + Concurrent modules |
| six-library | SIX-3.3.3 | auto-fetched if `SIX_ROOT` is not set |

### Quick start (six-library auto-fetched)

On the first configure, CMake clones and builds six-library (~500 MB download, ~10 min build). Subsequent builds use the cached result in `build/six-install/`.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build          # installs to <repo>/install/
```

Then launch with:

```bash
./install/bin/siv path/to/image.nitf
# or just
./install/bin/siv              # opens a file-picker dialog
```

### Using a pre-built six-library

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSIX_ROOT=/opt/six
cmake --build build --parallel
cmake --install build
```

### Install layout

```
install/
  bin/
    siv          # launcher script (sets LD_LIBRARY_PATH + QT_PLUGIN_PATH)
    SIV          # actual binary
  lib/           # bundled Qt + system .so files
  plugins/       # Qt platform + image-format plugins
  sample_images/ # cropped SICD/SIDD test files (auto-fetch builds only)
```

## Usage

| Action | Input |
|---|---|
| Open file | `File → Open` or pass path on command line |
| Zoom | Mouse wheel |
| Pan | Click and drag |
| Adjust stretch | Drag the **cyan** (black point) or **orange** (white point) handles on the histogram |
| View XML metadata | Expand the tree in the right-hand panel |

## Architecture

```
src/
  io/
    SARImageLoader.{h,cpp}   — loads overview + metadata via six-library
    TileProvider.{h,cpp}     — LRU tile cache, parallel tile-loader threads
  ui/
    ImageViewer.{h,cpp}      — QGraphicsView with smooth zoom/pan
    HistogramWidget.{h,cpp}  — log-scale histogram with draggable stretch handles
    MainWindow.{h,cpp}       — top-level window, dock, status bar
```

Tile loading uses one `NITFReadControl` per thread so reads are fully parallel. The tile size is chosen to match the NITF image segment's NPPBH/NPPBV block dimensions, keeping each `interleaved()` read aligned to a single file block.
