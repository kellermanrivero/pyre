# pyre

**Within ε, everywhere. Decode only what you need.**

pyre is a codec and a file format for terrain heights on a regular grid (a DEM, DTM or
DSM). It has a command-line tool. The method is an interpolative height pyramid with
closed-loop, ε-limited prediction and adaptive binary arithmetic coding, in independent
blocks of any size B.

- **A hard error limit.** No decoded height is more than ε from the source height. This
  is true at every node.
- **Random access.** A file is cut into tiles. The fine levels of a tile are cut into
  blocks. A reader decodes one block without the other blocks.
- **Level of detail.** The coarse levels come first. A reader can stop at a level and
  get an exact subset of the full grid.
- **No cracks.** Two neighbour tiles decode the same values on their shared side.
- **Bit-exact.** Integers only. The same file gives the same heights on ARM and x86.

The specification is `SPEC.md`. The method comes from a reference implementation, the
format 3 heightmaps of `heightmap.hpp`.

## The error rule

The codec rounds each source value to the quantum q (`--unit`, 1 cm by default). Then
it keeps each rounded value within ε. Thus the error against the float source is at most
**ε + q/2**. With ε = 0.99 m and q = 0.01 m, the limit is 0.995 m. With ε = 0 the
codec is lossless on the rounded values.

## Build

You need CMake 3.20 or later, a C++20 compiler and zlib (zlib is for the tool only).
libzstd is optional (the tool's reader of ZSTD TIFFs).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest
```

| CMake option | Default | Meaning |
|---|---|---|
| `PYRE_BUILD_TOOL` | ON | Build the `pyre` tool. |
| `PYRE_BUILD_TESTS` | ON | Build the tests. |
| `PYRE_SANITIZE` | OFF | Build with AddressSanitizer and UndefinedBehaviorSanitizer. |
| `PYRE_WITH_ZSTD` | ON | Read TIFF ZSTD compression (50000, as GDAL writes it) in the tool, if libzstd is found. Without it, such a file is refused with the reason. The codec never needs it. |
| `PYRE_REFERENCE_DIR` | empty | The folder of the reference `heightmap.hpp` (read only). |
| `PYRE_REFERENCE_NAMESPACE` | empty | The C++ namespace of the reference. If you set both and the header is there, `pyre bench --reference` and `test_reference` compare with it. |

The codec is `include/pyre/` only: headers, the C++20 standard library, no exceptions.
Copy the folder into a game (Unreal Engine, iOS, Android) as it is. This command checks
the build for iOS:

```sh
xcrun --sdk iphoneos clang++ -std=c++20 -fsyntax-only -Iinclude include/pyre/pyre.hpp
```

## The tool

```sh
pyre info   <file.pyre> [--tiles] [--json]
pyre encode <sources…> -o <out.pyre> [--epsilon 0.99] [--unit 0.01] [--tile 1000]
            [--block 200] [--base 8] [--block-levels 1] [--bbox minX,minY,maxX,maxY] [--threads N]
pyre decode <file.pyre> -o <out.tif> [--bbox …] [--level STEP] [--grid K] [--nodata V]
pyre verify <file.pyre> <sources…>
pyre bench  <sources…> [the encode options] [--reference] [--max-tiles N]
pyre compare <file.pyre> <pack.alt>
pyre encode <surface rasters…> --layer-of <file.pyre> --kind 1 [--unit 3] [--min 3] [--stride 2] [-o <out.pyre>]
```

Each command takes `--json` and then prints one JSON object.

- `encode` reads COG / GeoTIFF sources (TIFF and BigTIFF; strips and tiles; Deflate, LZW,
  ZSTD (with libzstd) or none; predictors 1, 2, 3; int16, int32, float32, float64). Each source becomes one
  grid of the file, on its own lattice, with no resampling.
- `decode` writes a float32 GeoTIFF. `--level` is a pyramid step in cells (1, 2, 4 … up
  to the base step). It is not in metres, because a source can be in degrees.
- `verify` decodes all tiles and checks each valid node against the float source. It
  exits with 1 on any breach.
- `bench` encodes and decodes in memory. With `--reference` it also encodes the same
  1001 × 1001 windows with the reference `heightmap.hpp` (the tiles with no holes only).

Example, with one MDT02 sheet:

```sh
pyre encode MDT02-WGS84-0559-2-COB2.tif -o madrid-ne.pyre --epsilon 0.99
pyre verify madrid-ne.pyre MDT02-WGS84-0559-2-COB2.tif
pyre decode madrid-ne.pyre -o madrid-ne-16.tif --level 8
```

## Layers: the height of objects above the ground

A file can carry layers: more integer grids on its tiles, each with its own stride,
quantum and error limit, coded as the heights are. A reader that needs only the
heights never reads them (`docs/FORMAT.md`, section 9). The kind (a u8) says what a
layer is; kind 1 is the height of objects (buildings, trees) above the ground.

A layer is added in a second pass, to a file that has its heights already:

```sh
pyre encode MDT02-WGS84-0559-2-COB2.tif -o 0559-2.pyre --epsilon 0.09        # pass 1: the heights
pyre encode <MDS02 folder> --layer-of 0559-2.pyre --kind 1                   # pass 2: the layer
pyre verify 0559-2.pyre --layer 1 <MDS02 folder>
pyre decode 0559-2.pyre --layer 1 -o 0559-2-objects.tif
scripts/add-layer.sh data/mdt02-e0.09 <MDS02 folder>                          # pass 2 on a folder
```

- **The ground is the file's own heights.** No terrain raster is needed in the second
  pass, and the surface sheets need not match the terrain sheets: any surface rasters
  in degrees are sampled at the file's nodes (the rule of `pyre-resample`). Under a
  hole of the heights (for example under a large roof) the ground is the decoder's
  prediction there, so the building still gets a height.
- **The values.** h = surface − ground. Each layer node takes the largest h of the
  `--stride` × `--stride` nodes round it (2: every 4 m on the MDT02). Under `--min`
  (3 m) the value is 0; else it is the nearest whole count of `--unit` (3 m), at least 1:
  3, 6, 9 … m, within 1.5 m. `--epsilon` (0) is the error limit in metres.
- **Without `-o`** the file is replaced through a new file and a rename. Its heights
  (and the layers it has) are copied byte for byte; the new layer's tiles go next to
  them.

The 8 test sheets, from the 9 cm archive, with the MDS02 (default options):

| Sheet | Heights (bytes) | Layer (bytes) | Added | Objects ≥ 3 m |
|---|---:|---:|---:|---:|
| 0559-2 city | 5,154,952 | 1,449,294 | 28.1 % | 38.7 % of the layer nodes |
| 0762-1 flat | 782,840 | 248,238 | 31.7 % | 5.2 % |
| 0737-2 flat | 5,844,248 | 1,308,807 | 22.4 % | 31.2 % |
| 0056-1 mountains | 16,619,176 | 1,933,783 | 11.6 % | 44.6 % |
| 0180-2 high mountains | 17,386,056 | 1,155,310 | 6.6 % | 25.3 % |
| 0014-1 coast | 3,022,296 | 468,828 | 15.5 % | 11.5 % |
| 0722-2 coast | 523,320 | 282,135 | 53.9 % | 7.2 % |
| 0146-1 border | 5,540,836 | 143,693 | 2.6 % | 9.7 % |
| all | 54,873,724 | 6,990,088 | 12.7 % | 22.8 % |

`verify --layer` checked every layer node against the MDS02 again: no error. The
surface model has trees as well as buildings, so kind 1 has both.

All of Spain (`scripts/add-layer.sh` on the 9 cm archive, 22 Oct 2026): all 4000 files
got the layer, none failed. The archive grew from 28,235,702,764 to 32,533,185,652 bytes
(+15.2 %; the layers are 4,266,935,497 bytes, 0.7431 bits a layer node). Of the
45,935,629,577 layer nodes, 13,014,965,445 (28.3 %) are objects of 3 m or more.
`verify --layer` checked all of them: no error.

38 sheets have an MDS02 sheet and no MDT02 sheet, and sheet 1057-3 has a damaged MDT02
TIFF. For these 39 the archive keeps the surface model as the heights (layer 0), with
no layer: `MDS02_WGS84_PM-2_<sheet>.pyre`, 274,915,736 bytes, verified (1,116,469,445
nodes, max error 9 quanta). Their heights are the top of buildings and trees, not the
ground. Do not run `scripts/add-layer.sh` on them: it would add a layer of zeros.

## Resampling onto a local metre grid: `pyre-resample`

`pyre encode` keeps each source on its own lattice. `pyre-resample` is a separate tool.
It puts many sources onto one metre grid round a centre point that you give, and then
it encodes that grid.

```sh
pyre-resample <sources or folders…> -o <out.pyre> --origin LAT,LON [--spacing 2] [--tile 1000]
              [--tiles-from <pack.alt> | --bbox minX,minY,maxX,maxY | --lonlat minLon,minLat,maxLon,maxLat]
              [--epsilon 0.99] [--unit 0.01] [--block 200] [--base 8] [--block-levels 1]
              [--tile-epsilon <file>] [--tile-epsilon-from <pack.alt>] [--overlay <raster or folder>]…
              [--threads N] [--open-limit 32] [--json]
```

- **The grid.** The projection is equirectangular on a sphere (R = 6,371,000 m), with
  its centre at `--origin`. Node (0, 0) is at the centre. A tile is `tile × spacing`
  metres, and tiles can be at negative places. The file stores the CRS as WKT.
  `tools/pyre/local.hpp` gives the arithmetic. It is the same as the reference packs
  use, so the sample points are the same.
- **The sampling.** For each node, the tool takes the sources that contain the point. It
  uses the one where the point is deepest inside, if the 4 pixels round the point have
  data. It interpolates bilinearly. If no source has data, the node is no-data. The
  choice depends on the point only, so two tiles that share a side get the same values
  there.
- **The sources** must be in degrees (a geographic CRS). A folder gives every `.tif`,
  `.tiff` and `.pyre` below it. Each grid of a `.pyre` file is a source (`file.pyre#K`
  names grid K), so an archive made with `pyre encode` can stand in for the original
  rasters (see below). The tool opens a source when a tile needs it, and keeps at most
  `--open-limit` open.
- **The tiles**: those of a reference pack (`--tiles-from`), or those in a box. Without
  either, it takes all the tiles that the sources touch.
- **An error limit for some tiles.** Some tiles can have their own ε (for example finer
  under airfields). The file stores it in the tile's index entry. `--tile-epsilon <file>`
  reads lines `tile <i> <j> <epsilon>` and `box <minLon> <minLat> <maxLon> <maxLat>
  <epsilon>` (every tile that touches the box). `--tile-epsilon-from <pack.alt>` takes the
  tiles that have their own error in a reference pack. Where rules meet, the smallest ε
  stays.
- **Overlays.** `--overlay <raster>` (you can give it again; a folder gives all its
  rasters) replaces samples before encoding: smoothed airfield ground, filled holes,
  corrections. An overlay must be on the grid: its pixel is the grid step, each pixel's
  sample point is a node, and its CRS is the same local grid or none. A pixel with no
  data leaves the sample. Where overlays meet, the last one wins. The error limit then
  holds against the overlay's values. `pyre decode` of a `pyre-resample` file writes a
  raster on the grid, so a tool can decode an area, change it, and give it back.
- **The check.** The tool decodes each tile again after it encodes it, and measures the
  error against the sampled values (with the overlays), each tile against its own limit.
  It exits with 1 on any breach.

`pyre compare <file.pyre> <pack.alt>` decodes a file from `pyre-resample` and a
reference heights pack (format 3, `.alt`) on the same grid. It prints the differences
and the sizes. pyre reads the pack with its own codec (the reference method), so it
needs no other code.

Example: Madrid. The reference pack has 2003 tiles round 40.5358125, −3.8213871. The
sources are all 4001 MDT02 sheets.

```sh
pyre-resample /Volumes/KELLERMAN/PublicData/mdt02 -o madrid.pyre --tiles-from madrid.alt \
              --tile-epsilon-from madrid.alt
pyre compare madrid.pyre madrid.alt
```

- `pyre-resample` wrote 2003 tiles (20,851,444 bytes), 26 of them with their own ε
  (0.09 m). It checked 2,006,979,229 nodes. No node was over its tile's limit, and the
  RMS was 0.400 m. The time was 13.2 s on 10 threads on a quiet machine, and 36 to
  58 s while other heavy programs ran. Two runs gave the same bytes.
- The pack keeps 1977 tiles at the pack's ε (0.99 m). Of these, 1950 tiles decode to the
  same heights as the pack, at every node. In the other 27 tiles the pack filled 28,774
  no-data nodes before it encoded them. pyre keeps those nodes as no-data. That changes
  the predictions near them: 14,314 nodes differ, by at most 1.970 m.
- The other 26 tiles have their own ε (0.09 m) in both files. They are airfield tiles:
  the pack's log says that their ground was smoothed before it was encoded. 86.8 % of
  their nodes are equal; the rest differ by the smoothing (RMS 0.198 m, max 7.86 m).
  Without `--tile-epsilon-from`, these tiles were at 0.99 m in pyre: then only 0.8 % of
  their nodes were equal (RMS 0.477 m, max 8.42 m).
- The sizes, with the index entries:

  | Tiles | Pack (bytes) | pyre (bytes) | Ratio |
  |---|---:|---:|---:|
  | The 1950 identical tiles | 18,846,398 | 18,619,319 | 0.9880 |
  | The 1977 tiles at the pack's ε | 19,053,174 | 18,835,012 | 0.9885 |
  | The 26 airfield tiles, 0.09 m in both | 2,001,510 | 2,015,754 | 1.0071 |
  | All 2003 tiles | 21,054,684 | 20,850,766 | 0.9903 |

  The airfield tiles are 0.7 % larger in pyre. The pack smoothed their ground, and a
  smooth ground costs fewer bits; pyre encodes the ground as the sheets give it.

### Working without the MDT02: an archive in pyre

To make the local grids again later (another centre, another ε, new airfields), keep an
archive of the sheets in pyre and delete the TIFFs. `pyre encode` keeps each sheet on its
own lattice, with no resampling, and `pyre-resample` reads the archive as it reads the
TIFFs.

```sh
scripts/archive.sh <MDT02 folder> data/mdt02-e0.09 0.09                    # once: one .pyre a sheet, verified
pyre-resample data/mdt02-e0.09 -o madrid.pyre --origin 40.5358125,-3.8213871 \
              --tile-epsilon airfields.txt --overlay airfield-flats/        # as often as needed
```

`scripts/archive.sh` runs `pyre encode` and then `pyre verify` on each raster, and writes
one `.pyre` a raster (X.tif gives X.pyre). It skips the rasters that are done, so a
stopped run goes on. A file that fails `verify` stays as `X.pyre.part`. Each raster gets
one JSON line in `archive.jsonl`. It stops when less than 20 GB are free.
`pyre encode <sheets…> -o one.pyre` makes one file with one grid a sheet instead.

All of Spain at ε = 0.09 m (`scripts/archive.sh`, 22 Oct 2026): 4000 of the 4001 MDT02
sheets, 28,235,702,764 bytes against 526,592,210,103 bytes of TIFF for the same sheets
(18.6 times smaller, 1.2569 bits a node). `verify` checked all 179,721,682,625 valid nodes:
max error 9 quanta (0.095 m against the float source), RMS 0.048 m, no breach. Sheet
1057-3 failed: its TIFF on the drive is damaged (19 of its 190 tiles are zeros, about
4 MB), so it must be downloaded again.

The MDT02 values are whole millimetres (checked on 3 sheets: 100 % of the values). A
reader of a pyre file gives each value as the float32 that a float32 raster holds. So
an archive with `--unit 0.001 --epsilon 0` gives back the TIFF values bit for bit, and
`pyre-resample` makes the same tiles from it: from the archive and from the 4001 TIFFs,
the Madrid file had the same bytes after the header (the header has the count of
sources).

The 167 sheets under the Madrid pack, 20,111,696,779 bytes of TIFF:

| Archive | Bytes | Smaller than the TIFFs | Error against the MDT02 | A file made from it |
|---|---:|---:|---|---|
| `--unit 0.001 --epsilon 0` | 7,865,124,400 | 2.6 × | 0 (bit for bit) | The same tiles as from the TIFFs |
| `--epsilon 0` (1 cm) | 4,777,312,728 | 4.2 × | ≤ 0.005 m | Of the 1977 tiles at the pack's ε, 81 identical to the pack; the others within 1.99 m of it (RMS 0.051 m) |
| `--epsilon 0.01` | 3,226,637,912 | 6.2 × | ≤ 0.015 m | |
| `--epsilon 0.1` | 1,006,184,264 | 20 × | ≤ 0.105 m | |
| `--epsilon 0.5` | 199,527,352 | 101 × | ≤ 0.505 m | |
| The game file (`madrid.pyre`, ε 0.99 m, 26 tiles at 0.09 m) | 20,851,444 | 964 × | ≤ 0.995 m against the samples | — |

The error of a file made from an archive adds up: at most the archive's error plus the
file's ε (bilinear sampling does not make the archive's error larger). Only the game
file is hundreds of times smaller. An archive that can make any later file exactly is
2.6 times smaller.

With the overlay as well, everything came from the mm archive: the airfield tiles were
within 0.36 m of the pack (RMS 0.017 m), against 7.86 m with no overlay; their size was
1.0159 of the pack's. The overlay here was a stand-in: the pack's own heights where it
differs from the MDT02 by more than 0.18 m (585,817 nodes), so it has sharp edges at
that threshold. AirKid's real flats will give its own airfield surfaces.

## Results

The measurements below come from an Apple M1 Pro (10 cores) with the test set
(`bench/sheets.txt`). The set is 8 IGN MDT02 sheets (2 m, WGS84 COG): a city, two flat
sheets, two mountain sheets, two coast sheets and one border sheet with 70 % no-data.
It has 387 tiles and 332,044,602 valid heights.

### Size, error and speed (`pyre bench`, all 8 sheets)

q = 1 cm, S = 1000, G = 8, blockLevels = 1. "bits / height" is the file size in bits
divided by the count of valid heights. The speeds count all 1001 × 1001 nodes of each
tile, also the no-data nodes. "head" is the mean time to decode the head of one tile.
"block" is the mean time to decode one block alone (one thread).

| ε (m) | B | bits / height | max error (m) | RMS (m) | encode Mnodes/s (1 / 10 threads) | decode Mnodes/s (1 / 10 threads) | head (µs) | block (µs) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.01 | 100 | 3.1633 | 0.015 | 0.0079 | 36 / 236 | 37 / 249 | 8417 | 184 |
| 0.01 | 200 | 3.0997 | 0.015 | 0.0079 | 37 / 243 | 38 / 261 | 8287 | 734 |
| 0.01 | 500 | 3.0858 | 0.015 | 0.0079 | 37 / 233 | 38 / 248 | 8328 | 4654 |
| 0.1 | 100 | 1.2593 | 0.105 | 0.0506 | 61 / 412 | 74 / 515 | 4323 | 91 |
| 0.1 | 200 | 1.2466 | 0.105 | 0.0506 | 62 / 422 | 75 / 518 | 4249 | 363 |
| 0.1 | 500 | 1.2468 | 0.105 | 0.0506 | 54 / 285 | 67 / 344 | 4593 | 2477 |
| 0.5 | 100 | 0.3885 | 0.505 | 0.2212 | 88 / 591 | 128 / 858 | 2490 | 53 |
| 0.5 | 200 | 0.3765 | 0.505 | 0.2212 | 89 / 584 | 129 / 878 | 2445 | 211 |
| 0.5 | 500 | 0.3731 | 0.505 | 0.2211 | 90 / 561 | 125 / 781 | 2442 | 1325 |
| 0.99 | 100 | 0.1946 | 0.995 | 0.4210 | 102 / 660 | 154 / 1023 | 2017 | 45 |
| 0.99 | 200 | 0.1823 | 0.995 | 0.4209 | 103 / 688 | 156 / 1038 | 1987 | 179 |
| 0.99 | 500 | 0.1779 | 0.995 | 0.4208 | 104 / 599 | 155 / 933 | 1990 | 1122 |
| 2 | 100 | 0.0931 | 2.005 | 0.8551 | 114 / 752 | 175 / 1139 | 1730 | 40 |
| 2 | 200 | 0.0822 | 2.005 | 0.8550 | 116 / 759 | 178 / 1172 | 1698 | 158 |
| 2 | 500 | 0.0781 | 2.005 | 0.8549 | 116 / 687 | 175 / 1035 | 1708 | 994 |

The max error is against the float source. In each row it is ε + q/2 exactly: the
error against the rounded source is D quanta at most, and never more. A smaller B costs
a little size and makes one block faster to decode. A block has B² · 3/4 nodes, so its
time grows with B².

### The error guarantee, per sheet (`pyre encode` + `pyre verify`)

All 48 runs (8 sheets × 6 values of ε) pass. `verify` checked 1,995,914,532 nodes, with
0 breaches and 0 mask errors. The table gives the bits per valid node of each file.
(Here the shared tile edges count twice, so the "all" row differs a little from the
table above.)

| Sheet | Terrain | valid nodes | ε = 0 | ε = 0.01 | ε = 0.1 | ε = 0.5 | ε = 0.99 | ε = 2 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 0559-2 | city (Madrid) | 44,416,783 | 4.6806 | 3.0534 | 0.8522 | 0.1770 | 0.0831 | 0.0392 |
| 0762-1 | flat (La Mancha) | 43,874,198 | 2.2472 | 0.9726 | 0.1296 | 0.0313 | 0.0192 | 0.0130 |
| 0737-2 | flat (La Mancha) | 44,397,344 | 4.9051 | 3.2516 | 0.9650 | 0.1394 | 0.0596 | 0.0329 |
| 0056-1 | mountains (Picos de Europa) | 45,736,289 | 7.4023 | 5.6904 | 2.7675 | 0.9321 | 0.4572 | 0.2057 |
| 0180-2 | high mountains (Pyrenees) | 46,546,023 | 7.4353 | 5.7499 | 2.8496 | 0.9950 | 0.4787 | 0.2015 |
| 0014-1 | coast (Gijón) | 46,873,459 | 2.4997 | 1.6442 | 0.4734 | 0.0924 | 0.0428 | 0.0222 |
| 0722-2 | coast (Valencia) | 46,689,600 | 0.6976 | 0.3746 | 0.0822 | 0.0205 | 0.0121 | 0.0085 |
| 0146-1 | border, 70 % no-data | 14,118,726 | 7.6541 | 5.9576 | 2.9969 | 1.0871 | 0.5365 | 0.2336 |
| all | | 332,652,422 | 4.4091 | 3.0940 | 1.2444 | 0.3758 | 0.1819 | 0.0820 |

The max error against the rounded source was exactly D quanta in each run (0, 1, 10,
50, 99 and 200).

### Against the reference (format 3)

`pyre bench --reference` encodes the same 1001 × 1001 windows with the reference
(ε = 0.99 m, the reference parameters). The reference has no no-data, so the comparison
uses the 178 tiles with no holes. The sizes include each tile's index entry: 32 B for
the reference; 48 B and the block ends (100 B) for pyre.

| Sheet | Tiles | Reference (bytes) | pyre (bytes) | Size ratio | Decode time ratio |
|---|---:|---:|---:|---:|---:|
| 0559-2 | 22 | 210,516 | 208,171 | 0.9889 | 0.997 |
| 0762-1 | 24 | 44,967 | 38,192 | 0.8493 | 0.989 |
| 0737-2 | 24 | 176,862 | 173,637 | 0.9818 | 1.014 |
| 0056-1 | 24 | 1,455,223 | 1,453,014 | 0.9985 | 1.025 |
| 0180-2 | 26 | 1,559,926 | 1,557,529 | 0.9985 | 1.035 |
| 0014-1 | 28 | 120,655 | 111,614 | 0.9251 | 0.993 |
| 0722-2 | 28 | 34,396 | 22,989 | 0.6684 | 0.985 |
| 0146-1 | 2 | 126,105 | 125,921 | 0.9985 | 1.013 |
| all | 178 | | | **0.9899** | **1.012** |

pyre decodes to the same heights as the reference. It is smaller because of the coder's
framing (method bit 0, section 6 of `docs/FORMAT.md`). Each stream drops its first byte,
which is always 0, and ends with the shortest flush. A flat tile has 1 + 25 streams, so
the saving is largest on flat land and sea. The decode times are the same within the
noise of the measurement (about ±3 % from one run to the next).

## Documents

- `SPEC.md`: the specification.
- `docs/FORMAT.md`: the file format, byte by byte.
- `docs/READERS.md`: the source reader interface, how to add a reader, and the resampler.
- `docs/REPORT.md`: the report on v0.1.
- `bench/sheets.txt`: the test set. `bench/sheets.sh` prints the full paths.

## Licence

MIT. See `LICENSE`.
