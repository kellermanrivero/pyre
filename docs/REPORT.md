# pyre v0.1 — the report

This report is for the owner. It follows section 10 of `SPEC.md`. All numbers come from
the tool output on an Apple M1 Pro (10 cores), macOS, Apple clang 21, CMake 4.4.

## 1. What was built

1. **The codec** (`include/pyre/`). It is header-only C++20, with the standard library
   only, no exceptions and integers only. It encodes and decodes one tile. Its
   parts are the head, the independent blocks, the level of detail and the no-data
   mask. The walk is one function for encode and decode. A block decode allocates
   nothing. Any S, B, G and blockLevels of section 4 work, with G up to 256.
2. **The file format** (`include/pyre/container.hpp`, `docs/FORMAT.md`). It has a
   header with the grids and their georeference, the tiles, and an index at the end.
   CRC-32C covers the header, the index and each tile. The reader works on a mapped
   file and checks every offset before use.
3. **The tool** `pyre` (`tools/pyre/`). Its commands are `info`, `encode`, `decode`,
   `verify` and `bench`, each with `--json`. It has its own COG / GeoTIFF reader
   (TIFF and BigTIFF) and a float32 GeoTIFF writer.
4. **The resampler** `pyre-resample` (`tools/pyre-resample/`), a separate tool. It
   puts many geographic sources onto one local metre grid round a centre point
   (`--origin LAT,LON`), and encodes it. The projection and the sampling rule are those
   of the reference packs. The owner asked for this (item 6). Section 9 of the
   specification put it out of v0.1.
5. **`pyre compare`** and a reader of the reference heights packs (`.alt`, format 3). The
   reader decodes the packs with pyre's own reference method.
6. **Per-tile ε and overlays in `pyre-resample`**, and **a pyre file as a source**
   (`PyreSource`). With these, an archive of the MDT02 in pyre replaces the TIFFs, and
   AirKid's airfield surfaces go into the file before it is encoded (section 7).
7. **Sections and layers** in format version 1. The reserved header bytes now point to a
   section table, so the format can grow; files without sections keep their bytes, and
   the Spain archive stays valid. A layer (section type 1) is another integer grid on
   the tiles with its own stride, quantum and error; its kind is a u8 (1: the height of
   objects above the ground). `pyre encode … --layer-of <file> --kind 1` adds it in a
   second pass from a surface model, with the file's own heights as the ground.
8. **The tests** (`tests/`): 5 suites with their own small harness and CTest.
9. **The documents**: `README.md`, `docs/FORMAT.md`, `docs/READERS.md` and this
   report.

### Decisions

| Point | Decision | Why |
|---|---|---|
| Language | C++20, not Rust | The owner asked for it. The codec goes into Unreal Engine. |
| Coder framing | Two methods: 0 is the reference's (LZMA framing); 1 is the default (trimmed) | Method 0 proves the port: its streams equal the reference's byte for byte. Method 1 is smaller (section 6). |
| B and G | B must be a multiple of 2^blockLevels, not of G | Then B = 100 and B = 500 are valid with G = 8. The block lines stay on the right lattice. |
| More than one sheet | One grid per sheet in one file. No resampling. | The sheets do not share a lattice (section 4 below). |
| `--level` | A step in cells (1, 2, 4 …), not "16m" | The MDT02 sheets are in degrees. A step in metres has no exact meaning there. |
| Subcommand names | As in the specification | There was no reason to change them. |
| Resampling | A separate tool, `pyre-resample`, with the reference packs' projection and sampling rule | The owner asked for a separate, generic tool with a centre point. With the same rule, its tiles can be compared with the packs node by node. |
| `compare` | `pyre compare <file.pyre> <pack.alt>`: the pack is a positional argument | `--reference` is already a flag of `bench`. |
| The quantiser | `round(v · 100)`, not `round(v / 0.01)`, when 1 / q is a whole number | One rounding, not two; the reference packs do the same. The ε sweep gave the same bytes before and after. |
| Overlays | `--overlay <raster>`: rasters on the grid whose values replace the samples before encoding | The owner chose this way (option 2) for the airfields: the error limit then holds against the smoothed ground, and a decoder needs no extra step. It also fills holes. |
| A pyre file as a source | `PyreSource`; a value is the float32 nearest to height × unit | So that the owner can delete the MDT02 and keep a pyre archive. The MDT02 values are whole mm, so an archive in mm (ε = 0) reads back bit for bit. |
| Layers | Sections in the reserved header bytes, not a version 2 | The owner wanted version 1 kept and room to grow. Old files and old readers keep working. |
| Height above ground | A second pass (`--layer-of`), not a second input of `encode` | The owner chose it: no pairs of MDT/MDS files. The ground is the decoded heights, so under a hole it is the prediction and buildings there keep a height. |
| Its values | The largest h in 2 × 2 nodes (4 m on the MDT02); 0 under 3 m; else whole steps of 3 m | The owner's choice: configurable, 3 m by default; the resolution follows the input. |
| Per-tile ε in `pyre-resample` | `--tile-epsilon <file>` (tiles or boxes in degrees) and `--tile-epsilon-from <pack.alt>`; the smallest ε wins | The owner asked for it, so that AirKid can keep tiles finer (airfields). The format already had a per-tile ε. |
| Local grid tiles | Node (0, 0) is the centre; tiles keep their places (negative too) through `tileI0`, `tileJ0` | A tile of the pyre file and a tile of the pack have the same (i, j). `decode` now handles a grid whose first tile is not (0, 0). |

## 2. The files

| File | What it is |
|---|---|
| `CMakeLists.txt` | The build: the codec target, the tool, the tests. |
| `include/pyre/pyre.hpp` | Includes all of the codec. |
| `include/pyre/params.hpp` | `Params`, `Status`, the limits. |
| `include/pyre/rc.hpp` | The binary range coder (15-bit probabilities, shift 5), with two framings. |
| `include/pyre/model.hpp` | The contexts and the residual code (zero flag, unary, Exp-Golomb). |
| `include/pyre/walk.hpp` | The pyramid walk for the head and for one block. |
| `include/pyre/tile.hpp` | `encodeTile`, `decodeHead`, `decodeBlock`, the quantiser. |
| `include/pyre/crc32c.hpp` | CRC-32C (ARM and SSE4.2 instructions, or a table). |
| `include/pyre/container.hpp` | `Builder` and `Reader` for the file. |
| `tools/pyre/main.cpp` | The argument parser. |
| `tools/pyre/commands.cpp`, `.hpp` | `info`, `encode`, `decode`, `verify`. |
| `tools/pyre/bench.cpp` | `bench`, with `--reference`. |
| `tools/pyre/source.hpp` | The source reader interface. |
| `tools/pyre/tiff.cpp`, `.hpp`, `tiff_codec.cpp`, `.hpp` | The COG / GeoTIFF reader (Deflate, LZW, predictors). |
| `tools/pyre/tiff_write.cpp` | The GeoTIFF writer for `decode`. |
| `tools/pyre/grid.cpp`, `.hpp` | From a source to a grid and its tiles. |
| `tools/pyre/files.hpp`, `json.hpp` | The mapped file, the file sink, the JSON writer. |
| `tools/pyre/local.hpp` | `LocalGrid`: the local metre grid round a centre point, and its WKT. |
| `tools/pyre/resample.cpp`, `.hpp` | `SheetSet`: many geographic sources, opened when needed, sampled onto the grid. |
| `tools/pyre/alt.cpp`, `.hpp` | `AltPack`: the reader of the reference heights packs. |
| `tools/pyre/compare.cpp` | `pyre compare`. |
| `tools/pyre/tile_epsilon.cpp`, `.hpp` | The per-tile ε of `pyre-resample`: from a file or a pack. |
| `tools/pyre/overlay.cpp`, `.hpp` | The overlays of `pyre-resample`. |
| `tools/pyre/pyre_source.cpp`, `.hpp` | `PyreSource`: one grid of a pyre file as a source. |
| `tools/pyre/source.cpp` | `openSource`: the reader for each file extension. |
| `tools/pyre/surface.cpp`, `.hpp` | Layer kind 1: the height above the ground, from a surface model and the file's heights. |
| `tools/pyre/layer.cpp` | The second pass (`--layer-of`) and `verify --layer`. |
| `scripts/add-layer.sh` | The second pass on a folder of `.pyre` files, each checked before it replaces the file. |
| `tools/pyre-resample/main.cpp` | The `pyre-resample` tool. |
| `tests/check.hpp`, `synth.hpp` | The test harness, and synthetic terrain. |
| `tests/test_codec.cpp` | The coder, the quantiser, the walk, random access, levels, cracks, damage, properties. |
| `tests/test_container.cpp` | The file format, the golden files, damaged files. |
| `tests/test_tiff.cpp` | The TIFF reader and writer. |
| `tests/test_reference.cpp` | Method 0 against the reference header, byte for byte. |
| `tests/test_resample.cpp` | The local grid, the sampling rule, the pack reader, the per-tile ε, the overlays, and a pyre file as a source. |
| `tests/golden/` | 3 small synthetic `.pyre` files with the CRC of their decoded heights. |
| `bench/sheets.txt`, `sheets.sh` | The test set, and a script that prints the full paths. |
| `docs/FORMAT.md`, `READERS.md`, `REPORT.md` | The documents. |

## 3. The dependencies

| Dependency | Where | Why |
|---|---|---|
| The C++20 standard library | The codec, the tool | — |
| zlib | The tool only | Deflate in the TIFF reader and writer. The macOS SDK has it. Nothing was downloaded. |
| Threads (pthreads) | The tool only | Encode and decode on many threads. |
| CMake 3.20 or later, CTest | The build | — |
| The reference `heightmap.hpp` | Optional, read only | `test_reference` and `bench --reference`. Set `PYRE_REFERENCE_DIR` and `PYRE_REFERENCE_NAMESPACE`. |

There is no test framework, no libtiff and no GDAL.

## 4. The sheet facts (section 6.2)

The four quarters of sheet 0559 (Madrid) were measured: 0559-1, -2, -3 and -4.

| Pair | Origin difference, fractional part (px) | Overlap | Points compared | Max / mean height difference (nearest pixel) |
|---|---|---|---|---|
| 1-2 | (−0.213, −0.304) | — | — | — |
| 1-3 (N–S) | (+0.105, −0.384) | 0.003157° = 175.4 px | 1,052,221 | 10.461 / 0.0638 m |
| 2-4 (N–S) | (+0.110, −0.358) | 0.002724° = 151.4 px | 959,102 | 8.522 / 0.0570 m |
| 3-4 (E–W) | (−0.208, −0.279) | 163.2 px | 544,591 | 10.348 / 0.0496 m |
| 1-4 (corner) | (−0.103, +0.338) | 163 × 164 px | 8,754 | 0.661 / 0.0183 m |
| 2-3 (corner) | (+0.317, −0.080) | 163 × 164 px | 11,256 | 1.498 / 0.0156 m |

- The sheets do not share one lattice. The offsets are 0.08 to 0.38 px.
- Neighbours overlap by 151 to 175 px (about 250 m).
- In the overlaps the heights agree to 5 or 6 cm on average. Local differences go up to
  about 10 m.
- The four corners of each sheet are no-data.

Thus a file holds one grid for each sheet. The overlaps are stored twice.

## 5. The test results

| Suite | Result |
|---|---|
| `test_codec` | 20 tests, 0 failed |
| `test_container` | 12 tests, 0 failed |
| `test_tiff` | 13 tests, 0 failed |
| `test_resample` | 17 tests, 0 failed |
| `test_reference` | 1 test, 0 failed |
| All 5 under AddressSanitizer and UndefinedBehaviorSanitizer (`build-asan`) | 100 % passed (249.3 s) |
| `pyre-resample` and `pyre compare` on all of Madrid (2003 tiles, 4001 sheets) under the sanitizers | No report. The file is the same, byte for byte, as from the normal build; `compare` gives the same result. |
| iOS: `xcrun --sdk iphoneos clang++ -std=c++20 -fsyntax-only` on `pyre.hpp` | No error |

The error guarantee on the test set: `encode` and then `verify` for each of the 8
sheets and each ε in {0, 0.01, 0.1, 0.5, 0.99, 2} m. All 48 runs pass. `verify`
checked 1,995,914,532 nodes, with 0 breaches and 0 mask errors. In each run the max
error against the rounded source is exactly D quanta, and against the float source
ε + q/2. The README has the table.

## 6. Acceptance (section 8)

| # | Requirement | Result |
|---|---|---|
| 1 | All tests pass on macOS. The codec headers compile for iOS. | **Pass.** See section 5. |
| 2 | ε = 0.99 m, q = 1 cm, S = 1000, B = 200, G = 8, blockLevels = 1: size not larger than the reference, decode not more than 10 % slower | **Pass.** Size ratio 0.9899, decode time ratio 1.012 (all 178 tiles with no holes). See the table below. |
| 3 | The README table (ε × B) | **Pass.** 15 rows on all 8 sheets (387 tiles). |
| 4 | `docs/FORMAT.md`, `docs/READERS.md`, `docs/EXPERIMENTS.md` | **Partly.** FORMAT and READERS are written. The owner chose to skip the experiments, so `EXPERIMENTS.md` is not written. |

The comparison method for item 2: `pyre bench --reference` encodes the same
1001 × 1001 windows with the reference header and with pyre. The reference has no
no-data, so only the tiles with no holes are compared. The sizes include the index
entries (32 B a tile for the reference; 48 B and 100 B of block ends for pyre).

| Sheet | Terrain | Tiles | Size ratio (pyre / reference) | Decode time ratio |
|---|---|---:|---:|---:|
| 0559-2 | city | 22 | 0.9889 | 0.997 |
| 0762-1 | flat | 24 | 0.8493 | 0.989 |
| 0737-2 | flat | 24 | 0.9818 | 1.014 |
| 0056-1 | mountains | 24 | 0.9985 | 1.025 |
| 0180-2 | high mountains | 26 | 0.9985 | 1.035 |
| 0014-1 | coast | 28 | 0.9251 | 0.993 |
| 0722-2 | coast | 28 | 0.6684 | 0.985 |
| 0146-1 | border, 70 % no-data | 2 | 0.9985 | 1.013 |
| all | | 178 | **0.9899** | **1.012** |

An earlier run on 200 tiles (with 0001-2 in place of 0146-1) gave 0.9868 and 0.988.
The decode time ratio changes by about ±3 % from run to run.

## 7. The resampler against a reference pack (Madrid)

`pyre-resample` read all 4001 MDT02 sheets and made the 2003 tiles of the Madrid pack
(`madrid.alt`, centre 40.5358125, −3.8213871). Then `pyre compare` decoded both.

| What | Result |
|---|---|
| `pyre-resample` (one ε) | 2003 tiles, 18,996,292 bytes, 13.2 s on 10 threads. 2,006,979,229 nodes checked: max error 99 quanta (0.995 m against the samples), RMS 0.403 m. |
| `pyre-resample --tile-epsilon-from madrid.alt` | 2003 tiles, 26 at 0.09 m: 20,851,444 bytes. No node over its tile's limit; RMS 0.400 m. The same bytes on a second run. 36 to 58 s while other heavy programs ran (load average about 40); 13.2 s for the one-ε run on a quiet machine. |
| Tiles at the pack's ε (0.99 m) | 1977. 1950 decode to the same heights as the pack at every node. |
| The other 27 of them | The pack filled 28,774 no-data nodes in them; pyre keeps them as no-data. 14,314 nodes differ, by at most 1.970 m. |
| Tiles with the pack's own ε (0.09 m) | 26 airfield tiles. Their ground was smoothed before encoding (the pack's log); pyre does not do this. With one ε in pyre: 0.8 % of their nodes equal, RMS 0.477 m, max 8.42 m. With the same per-tile ε: 86.8 % equal, RMS 0.198 m, max 7.86 m. |
| Size, the 1977 tiles at the pack's ε | Pack 19,053,174 bytes, pyre 18,835,012 bytes: ratio 0.9885. |
| Size, the 1950 identical tiles | Ratio 0.9880. |
| Size, the 26 airfield tiles at 0.09 m in both | Pack 2,001,510 bytes, pyre 2,015,754 bytes: ratio 1.0071 (the pack's ground there is smoothed, so it costs less). |
| Size, all 2003 tiles, the same per-tile ε | Pack 21,054,684 bytes, pyre 20,850,766 bytes: ratio 0.9903. |

Thus the resampler takes the same sample points, chooses the same sheets and gets the
same values as the reference. Where the heights are the same, pyre's file is 1.2 %
smaller.

**Without the MDT02.** The 167 sheets under the pack (20,111,696,779 bytes of TIFF) were
put into one pyre archive with `pyre encode --unit 0.001 --epsilon 0`: 7,865,124,400 bytes.
`pyre-resample` from the archive alone made the same tiles and index, byte for byte, as
from the 4001 TIFFs (only the header differs: it names the count of sources). Coarser
archives are smaller, but their error adds to the file's:

| Archive | Bytes | Against the TIFFs |
|---|---:|---:|
| mm, ε = 0 | 7,865,124,400 | 2.6 × smaller, exact |
| cm, ε = 0 | 4,777,312,728 | 4.2 × smaller, ≤ 0.005 m |
| ε = 0.01 m | 3,226,637,912 | 6.2 × smaller, ≤ 0.015 m |
| ε = 0.1 m | 1,006,184,264 | 20 × smaller, ≤ 0.105 m |
| ε = 0.5 m | 199,527,352 | 101 × smaller, ≤ 0.505 m |

**With an overlay.** A stand-in for AirKid's airfield flats: the pack's heights where it
differs from the MDT02 by more than 0.18 m, 585,817 nodes in 26 GeoTIFFs. From the mm
archive with this overlay and the pack's per-tile ε, the 26 airfield tiles came within
0.36 m of the pack (RMS 0.017 m; 94.3 % of the nodes equal), against 7.86 m with no overlay.
Their size was 1.0159 of the pack's: the stand-in has sharp edges at the 0.18 m threshold.

## 8. Better or worse than the reference, and why

Better:

1. **Smaller.** The trimmed framing removes the first byte of each stream (always 0)
   and writes the shortest flush. A tile has 26 streams. Thus the saving is about
   100 bytes a tile. That is a large part of a flat or sea tile (0722-2: −33 %) and a
   small part of a mountain tile (−0.15 %).
2. **No-data.** The reference has no no-data. It must fill the holes before it
   encodes. pyre stores a mask in the same coder. A tile with no holes pays 1 bit.
3. **All parameters.** S, B, G and blockLevels are free (section 4). The reference
   has 1001 nodes, 8 and 200 fixed.
4. **Checks.** The file has CRCs, a version, and full bounds checks. Damaged files
   give an error. The tests also run under the sanitizers.
5. **Georeference.** The file holds the CRS, the origin, the spacing and the raster
   type of each grid. The reference has only an origin in degrees.
6. **Random access without decoding.** The index gives the byte range of each block.
   The reference must read a table at the start of each tile.

The same:

- The heights. Method 0 gives the same streams. Method 1 gives the same decoded
  heights.
- The decode speed (ratio 1.012, within the noise).

Worse:

- **The index is larger**: 48 B and 4 B a block for each tile (148 B with B = 200),
  against 32 B. The size ratios above include this cost.
- **With method 1** a cut stream is not always seen by the decoder alone. The tile's
  CRC finds it.

## 9. Open points

1. **`SPEC.md` is not committed.** The owner asked for this. The local commit was
   amended (`837bf73` → `052efd2`) and `.gitignore` now has `/SPEC.md`.
2. **`docs/EXPERIMENTS.md` and the experiments** (cubic prediction, contexts, context
   mixing, an error that changes by level) are not done. The owner chose to skip them.
3. **`bench/sizes.py` and `bench/verify_all.sh`** are not mine. They are not tracked.
   `sizes.py` needs `bench --size-only` and `--method`, which the tool does not have.
4. **`pyre encode` with many sheets** stores the overlaps twice (one grid a sheet).
   `pyre-resample` is the way to one grid.
5. **The airfield flats** must come from AirKid: pyre has the overlay, not the
   smoothing. Only a stand-in was tested.
6. **The size of the archive** is the owner's choice (the table in section 7): exact
   (mm) is 2.6 times smaller than the TIFFs; hundreds of times smaller is only possible
   for the game files, or for an archive with an error of about 0.5 m.
7. **Layers are not carried by `pyre-resample` yet.** A local grid made from an archive
   with layers has the heights only. The second pass works on a local grid too (the
   surface is sampled at its nodes).
8. **The coarse levels of a layer are samples**, as for the heights. Far-away buildings
   would need the largest value of each cell instead: a later codec option.
9. **Layer kind 1 has trees as well as buildings**, because the MDS02 has both.
10. **The resampler reads geographic sources only.** A projected source needs a CRS
   transform, which is not built.
11. **The GeoTIFF of a local grid** carries the WKT as a citation only (no GeoKeys for
   the projection). GDAL is not on this machine, so I could not check how GDAL reads it.
12. **No-data on a local grid** stays no-data unless an overlay gives values. The
   reference fills those nodes from the data round them, and smooths the ground under
   airfields; with pyre, AirKid gives both as overlays, and keeps the airfield tiles
   finer with a per-tile ε. The holes are under large
   buildings (31 of 32 patches in Madrid; the MDS02 is 8 to 37 m above the ground there).
   A decoder already gets a smooth value under each hole (its prediction), except a hole
   at a tile corner, which decodes to 0.
13. **A `Source` for the `.alt` packs** (to convert them to pyre files) is not built. The
   pack reader is there.
14. **The slowest single block** in a run is noisy (up to 15.8 ms in one run with
   B = 500, against a mean of 2.5 ms). The README gives only the means.
15. **Section 11, question 2** (pyre in the game) is for the owner.
