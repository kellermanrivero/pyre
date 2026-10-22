# Source readers

The tool reads source rasters through one interface: `pyre::tool::Source` in
`tools/pyre/source.hpp`. Each format has one reader. The first reader is COG / GeoTIFF
(`tools/pyre/tiff.cpp`).

## 1. The layers

```
reader        reads a window of source values and gives the metadata   (source.hpp, tiff.cpp …)
grid layer    makes the pyre grid and cuts the tiles                    (grid.hpp, grid.cpp)
codec         encodes and decodes the tiles, writes and reads the file  (include/pyre)
```

A reader does not cut tiles. A reader does not resample. Keep these jobs apart. A
resampler (for example onto the AirKid metre grid) is a later step: it will replace
`readTile` in the grid layer. The readers and the codec will not change.

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
4. Add the file extension to `openSource` (in `tiff.cpp` now; move it to its own
   `source.cpp` when a second reader comes).
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
| mundosm `.alt` | The reference packs. A reader decodes a tile with `heightmap.hpp` and gives the AirKid metre grid: projected CRS, PixelIsPoint, 2 m. Use it to convert the reference packs. |

## 5. More than one source

One source is one grid of the file. Each tile belongs to one grid. Tiles start at the
source's south-west pixel. `pyre encode` takes many sources and writes one grid for
each. It does not check whether two sources overlap: the MDT02 sheets overlap by about
250 m, and both copies are stored. A later resampler will put many sources onto one
grid.
