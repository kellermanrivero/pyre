# Source readers

The tool reads source rasters through one interface: `pyre::tool::Source` in
`tools/pyre/source.hpp`. Each format has one reader:

| Reader | Files | What it reads |
|---|---|---|
| `TiffSource` (`tiff.cpp`) | `.tif`, `.tiff` | COG / GeoTIFF |
| `PyreSource` (`pyre_source.cpp`) | `.pyre`, `.pyre#K` | One grid of a pyre file, so that a pyre archive can stand in for the rasters it was made from |

`openSource` (`source.cpp`) chooses the reader by the file extension.

## 1. The layers

```
reader        reads a window of source values and gives the metadata   (source.hpp, tiff.cpp …)
grid layer    makes the pyre grid and cuts the tiles                    (grid.hpp, grid.cpp)
codec         encodes and decodes the tiles, writes and reads the file  (include/pyre)
```

A reader does not cut tiles. A reader does not resample. Keep these jobs apart. The
resampler (`resample.hpp`, used by `pyre-resample`) sits in the grid layer, in place of
`readTile`. It uses the readers as they are (section 6).

## 2. The interface

```cpp
class Source {
public:
    virtual ~Source() = default;
    virtual const SourceInfo& info() const = 0;
    virtual void readWindow(std::uint32_t col0, std::uint32_t row0,
                            std::uint32_t width, std::uint32_t height, double* out) const = 0;
};
std::unique_ptr<Source> openSource(const std::string& path);
```

`SourceInfo` gives:

| Field | Meaning |
|---|---|
| `path` | The file. The grid of the pyre file takes its base name. `verify` matches sources to grids by this name. |
| `width`, `height` | The raster size in pixels. |
| `sample` | The sample type in the file. |
| `georef` | The CRS (EPSG code, or WKT), the vertical CRS and unit, the raster type (PixelIsArea or PixelIsPoint), the origin and the spacing. |
| `noData` | The no-data value, if the source has one. |

The rules for a reader:

1. Pixel (0, 0) is the first pixel of the file (normally the north-west pixel).
   Rows go south when `georef.spacingY` is positive.
2. `georef.originX`, `originY` is the reference point of pixel (0, 0): its north-west
   corner for PixelIsArea, the sample itself for PixelIsPoint.
3. `readWindow` writes `width × height` values, row-major, the first row first.
4. Values are `double`. A no-data pixel keeps its raw value. `SourceInfo::isNoData`
   says which values are no-data (NaN is always no-data).
5. `readWindow` is `const`, and it must be safe on many threads at once. Use `pread`
   or a mapped file, not a shared file position.
6. Read only the parts of the file that the window needs, if the format allows it.
7. A damaged file must give a `SourceError`, never a crash. Check every offset and
   size against the file size.

## 3. Add a reader

1. Write `tools/pyre/<format>.hpp` and `.cpp` with a class that derives from `Source`.
2. In the constructor (or a static `open`), read the metadata and fill `SourceInfo`.
   Throw `SourceError` with a clear message if the file is not valid.
3. Write `readWindow`. Convert each sample to `double`.
4. Add the file extension to `openSource` in `source.cpp`.
5. Add the `.cpp` to the `pyretool` library in `CMakeLists.txt`.
6. Add `tests/test_<format>.cpp`: small files made by the test, windows, no-data, and
   damaged files.
7. Run `pyre encode` and `pyre verify` on a real file of the format.

## 4. Notes for the planned readers

| Format | Notes |
|---|---|
| ESRI ASCII Grid (`.asc`) | Text. `xllcorner`/`xllcenter` give PixelIsArea or PixelIsPoint. Read the whole file at open; it is small. |
| SRTM HGT | Big-endian int16, 1201² or 3601², no-data −32768. The name gives the position. PixelIsPoint. |
| BIL / BSQ with `.hdr` | Raw samples. The header gives the size, the type, the byte order and the position. One band. |
| USGS DEM | Text, profiles of columns. Convert it to rows in the reader. |
| DTED | Big-endian, sign-magnitude int16, columns south to north. Convert to rows. |
| NetCDF, Zarr | Need libraries (netCDF/HDF5, a JSON parser and a codec for Zarr). Keep them optional in CMake. |
| Copernicus DEM | COG: the GeoTIFF reader reads it now. |
| LERC | Needs the LERC library (Apache-2.0). Optional in CMake. |
| Reference `.alt` packs | Format 3 heights packs (magic `MALT`). `tools/pyre/alt.hpp` reads them now and decodes a tile with pyre's reference method (`Method::reference()`, the same streams); `pyre compare` uses it. A `Source` on top of it (to convert the packs) is still to do. |

## 5. More than one source

One source is one grid of the file. Each tile belongs to one grid. Tiles start at the
source's south-west pixel. `pyre encode` takes many sources and writes one grid for
each. It does not check whether two sources overlap: the MDT02 sheets overlap by about
250 m, and both copies are stored. To put many sources onto one grid, use
`pyre-resample` (section 6).

## 6. The resampler

`pyre-resample` puts many geographic sources onto one local metre grid. Its parts:

| File | What it does |
|---|---|
| `tools/pyre/local.hpp` | `LocalGrid`: the projection round a centre point, and its WKT. |
| `tools/pyre/resample.hpp`, `.cpp` | `SheetSet`: reads the headers of the sources, opens them when a tile needs them, and samples a square of nodes. |
| `tools/pyre/overlay.hpp`, `.cpp` | `Overlay`: rasters on the grid whose values replace the samples. |
| `tools/pyre/tile_epsilon.hpp`, `.cpp` | The per-tile ε: from a file of tiles and boxes, or from a pack. |
| `tools/pyre-resample/main.cpp` | The tool: chooses the tiles, samples, applies the overlays, encodes and checks. |

The rules of `SheetSet::sample`:

1. It computes the degrees of each column and each row of nodes (on this grid, the
   longitude depends on x only and the latitude on y only).
2. It finds the sources whose box of pixel centres touches the square. It reads one
   window from each of them.
3. For each node, it orders those sources by the margin: the distance in pixels from
   the node to the nearest edge of the box. The largest margin comes first. On a tie,
   the first path comes first.
4. It takes the first source whose 4 pixels round the node all have data. It
   interpolates bilinearly in double and rounds the result to float.
5. A pixel has no data if it is the source's no-data value, NaN, infinite, or less than
   −1000 m.

A source for the resampler must be geographic (`ModelType::Geographic`) and have rows
going south. Any reader of section 3 can give it. A source in a projected CRS needs a
CRS transform first; that is not done.

## 7. A pyre file as a source

`PyreSource` reads one grid of a pyre file as a raster:

- The path is `file.pyre` (a file with one grid) or `file.pyre#K` (grid K). `SheetSet`
  gives each grid of a file its own source, in the order of the grids.
- Pixel (0, 0) is the grid's north-west node. The georeference is the grid's.
- A value is the float32 nearest to height × unit, as a float32 raster holds it. So a
  float32 source whose values are whole quanta, encoded with that unit and ε = 0, reads
  back bit for bit. The MDT02 values are whole millimetres: use `--unit 0.001`.
- A hole, or a node of a tile that is not in the file, reads as NaN.
- It decodes whole tiles and keeps the last 16 (an LRU cache). It checks each tile's CRC
  before it decodes it.
