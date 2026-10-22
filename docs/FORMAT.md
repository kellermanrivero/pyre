# The pyre file format, version 1

This document gives the `.pyre` file byte by byte. The code that writes and reads it is
`include/pyre/container.hpp` (`Builder`, `Reader`). The code of the streams is
`include/pyre/tile.hpp`, `walk.hpp`, `model.hpp` and `rc.hpp`.

## 1. General rules

- All numbers are little-endian.
- `u8`, `u16`, `u32`, `u64` are unsigned integers. `i32` is a signed integer (two's
  complement). `f64` is an IEEE 754 double.
- Each field is on a multiple of its size from the start of the file. A reader can
  use a mapped file as it is. No offset needs a fix.
- All offsets are absolute: they count bytes from the start of the file.
- Reserved bytes are 0. A writer writes 0. A reader ignores them, except where this
  document says otherwise.
- The checksum is CRC-32C (Castagnoli, the polynomial 0x1EDC6F41, reflected, with
  initial value and final XOR 0xFFFFFFFF). The check value of `"123456789"` is
  `0xE3069283`.

## 2. The layout

```
offset 0             the header: the fixed part (128 B), the grids (128 B each), the text
headerBytes          the tiles, one after another; each tile is its head, then its blocks
indexOffset          the index: the entries (48 B each), then the block ends (4 B each)
fileBytes            the end of the file
```

The index is at the end. A writer can then stream the tiles, and write the index and
the header last. The header gives the place of the index. To read one block over
HTTP, a client needs three range reads: the header, the index, then the block.

## 3. The header

### 3.1 The fixed part (128 bytes)

| Offset | Type | Name | Meaning |
|---:|---|---|---|
| 0 | u8[4] | magic | `P` `Y` `R` `E` |
| 4 | u32 | version | 1. A reader refuses a version that it does not know, and says why. |
| 8 | u32 | headerBytes | The size of the whole header (the fixed part, the grids and the text), a multiple of 8. The first tile starts here. |
| 12 | u32 | headerCrc | CRC-32C of bytes [0, headerBytes), with this field set to 0. |
| 16 | u32 | tile | S: the tile side in cells. A tile has S + 1 nodes on each side. |
| 20 | u32 | block | B: the block side in cells. |
| 24 | u32 | base | G: the coarsest pyramid step in cells. |
| 28 | u32 | blockLevels | The count of the finest levels that are cut into blocks. |
| 32 | u32 | method | The coding method (section 6). A reader refuses bits that it does not know. |
| 36 | i32 | epsilon | D: the file's error limit, in quanta. A tile can have its own (section 4). |
| 40 | f64 | unit | q: the height quantum, in the vertical unit (for example 0.01 m). |
| 48 | f64 | noData | The source's no-data value, for output files. NaN if the source has none. |
| 56 | u32 | gridCount | The count of grids. At least 1. |
| 60 | u32 | flags | 0. A reader refuses other values. |
| 64 | u64 | tileCount | The count of tiles (and of index entries). |
| 72 | u64 | indexOffset | Where the index starts. A multiple of 8. |
| 80 | u64 | indexBytes | The size of the index: tileCount × (48 + 4 × blocks a tile). |
| 88 | u32 | indexCrc | CRC-32C of the index (all indexBytes). |
| 92 | u32 | — | Reserved. |
| 96 | u64 | fileBytes | The size of the file. A reader uses it to find a cut file. |
| 104 | u8[24] | — | Reserved. |

The rules of the parameters (a reader refuses a file that breaks them):

- G is a power of 2, from 2 to 256.
- blockLevels is from 1 to log2(G).
- S is from 1 to 16384, and S is a multiple of G and of B.
- B is a multiple of 2^blockLevels. (B need not be a multiple of G. B = 100 and
  B = 500 with G = 8 are valid. The block lines need only be on the finest grid of the
  head.)

The blocks a tile: (S / B)².

### 3.2 The grids (128 bytes each)

The grids follow the fixed part: grid k is at 128 + 128 × k. A grid is a lattice of
nodes in one CRS. Each tile belongs to one grid. The MDT02 sheets do not share a
lattice, so each sheet is one grid, and pyre does not resample.

| Offset | Type | Name | Meaning |
|---:|---|---|---|
| 0 | u32 | epsg | The horizontal CRS as an EPSG code. 0 if there is no code: then see `wkt`. |
| 4 | u32 | verticalEpsg | The vertical CRS as an EPSG code. 0 if not known. |
| 8 | u8 | rasterType | 1 = PixelIsArea, 2 = PixelIsPoint (the GeoTIFF codes). |
| 9 | u8 | modelType | 0 unknown, 1 projected, 2 geographic, 3 geocentric. |
| 10 | u16 | — | Reserved. |
| 12 | u32, u32 | wkt | The offset and size of the CRS as WKT (or a citation) in the text. 0, 0 if none. |
| 20 | u32, u32 | verticalUnit | The offset and size of the vertical unit ("metre"). 0, 0 if none. |
| 28 | u32, u32 | name | The offset and size of the grid's name (the source file's name). |
| 36 | u32 | width | The nodes with data across, from node (0, 0). |
| 40 | u32 | height | The nodes with data up, from node (0, 0). |
| 44 | i32 | tileI0 | The first tile column (0 for a sheet). |
| 48 | i32 | tileJ0 | The first tile row (0 for a sheet). |
| 52 | u32 | tilesX | The tile columns. |
| 56 | u32 | tilesY | The tile rows. |
| 60 | u32 | — | Reserved. |
| 64 | f64 | originX | The CRS position of node (0, 0). |
| 72 | f64 | originY | |
| 80 | f64 | spacingX | The distance between two nodes, in CRS units. |
| 88 | f64 | spacingY | |
| 96 | u8[32] | — | Reserved. |

The place of a node: node (I, J) of a grid is at
(originX + I × spacingX, originY + J × spacingY). J goes north. Node (0, 0) is the
south-west node. For PixelIsPoint the position is the sample. For PixelIsArea it is
the north-west corner of the node's cell, as in GeoTIFF.

The tiles of a grid: tile (ti, tj) holds the nodes (I, J) with
ti × S ≤ I ≤ ti × S + S and tj × S ≤ J ≤ tj × S + S. Two neighbour tiles share the
nodes of their common side.

### 3.3 The text

The text follows the grids. It is UTF-8 with no terminating zero. The grids point into
it. Zero bytes pad the header to a multiple of 8.

## 4. The index

The index starts at indexOffset. It has two parts.

### 4.1 The entries (48 bytes each)

The entries are in order: by grid, then by tile row tj (south to north), then by tile
column ti (west to east). No two entries have the same grid, ti and tj. A reader finds
a tile with a binary search.

| Offset | Type | Name | Meaning |
|---:|---|---|---|
| 0 | u32 | grid | The grid of the tile. |
| 4 | i32 | ti | The tile column. |
| 8 | i32 | tj | The tile row. |
| 12 | u32 | flags | Bit 0: the tile has holes (no-data nodes). Other bits are 0; a reader refuses others. |
| 16 | u64 | offset | Where the tile's bytes start. |
| 24 | u32 | headBytes | The size of the head stream. |
| 28 | u32 | totalBytes | The size of the tile: the head and all its blocks. |
| 32 | i32 | min | The smallest decoded height of the valid nodes, in quanta. |
| 36 | i32 | max | The largest decoded height of the valid nodes, in quanta. |
| 40 | i32 | epsilon | The tile's error limit D, in quanta. It is the header's value unless the tile has its own. |
| 44 | u32 | crc | CRC-32C of the tile's bytes (all totalBytes). |

A tile with no valid node is not stored.

### 4.2 The block ends (4 bytes each)

After the entries: for each entry, in the same order, (S / B)² values of type u32.
Value b is the end of block b, counted from the start of the tile. The blocks are in
rows from south to north; each row goes from west to east. So block (bx, by) is
block b = by × (S / B) + bx.

The byte range of block b in the file:

```
begin = offset + (b == 0 ? headBytes : end[b - 1])
end   = offset + end[b]
```

A reader gets the range of one block from the index only. It decodes nothing.
The last end is totalBytes.

## 5. A tile

A tile is its head stream, then its block streams, in the order of the blocks.
Each stream is a separate arithmetic-coder stream with its own model. A block stream
needs the head only, never another block.

### 5.1 The walk

`include/pyre/walk.hpp` is the authority. The heights are integers in quanta, so all
platforms decode the same values. The order of a tile (L = log2 G):

The head:

1. The four corners: (0, 0), (S, 0), (0, S), (S, S).
2. The four sides: south, north (west to east), west, east (south to north). For each
   side: first the nodes at multiples of G, each predicted by the node G before it
   (DPCM). Then each level l = 1 … L: the nodes at odd multiples of G / 2^l, each
   predicted by the mean of its two neighbours on the side. A side uses its own nodes
   only, so two tiles that share it decode the same values.
3. The inside of the base grid (step G), rows south to north, each west to east. The
   prediction is MED (JPEG-LS) of the west, south and south-west nodes.
4. Each level l = 1 … L − blockLevels, with h = G / 2^l:
   the centres (both coordinates odd multiples of h) from the mean of their four
   corners, then the edge midpoints from the mean of their four neighbours in a
   diamond.
5. Each block level l = L − blockLevels + 1 … L: the level's nodes on the inner block
   lines (x = b × B and y = b × B, 0 < b < S / B), each predicted by the mean of its
   two neighbours on the line. For each b: first the vertical line, then the
   horizontal line.

Each block: each block level, as in step 4, inside the block's lines.

Rounding: `mean2(a, b) = (a + b + 1) >> 1`, `mean4(a, b, c, e) = (a + b + c + e + 2) >> 2`,
in 64-bit integers (arithmetic shift).

A decoder can stop at a step s (a power of 2 up to G). It then has all the nodes on
the grid of step s. The head stops before the first level with a step finer than s.
A block stops in the same way.

### 5.2 One node

The difference to the prediction is quantised (JPEG-LS near-lossless):

```
r = x − pred,  step = 2D + 1
q = r ≥ 0 ? (r + D) / step : −((D − r) / step)       (integer division)
decoded = pred + q × step                             so |decoded − x| ≤ D
```

The value q is coded with its context (see `model.hpp`):

1. zigzag: z = 2q for q ≥ 0, else −2q − 1;
2. a bit "z ≠ 0";
3. if z ≠ 0, v = z − 1 in unary, up to 15 bits, each with its own probability;
4. if v ≥ 15, the order-0 Exp-Golomb code of v − 15: w = v − 14, n = ⌊log2 w⌋;
   n one-bits with probabilities, a zero-bit with a probability (not written when
   n = 31), then the n low bits of w with probability 1/2.

The contexts: 12 bands of local activity (the spread of the predicting nodes, in
doublings of D). For 2D nodes: level × (centre or edge midpoint) × band. For sides and
lines: level × band.

A corner is 32 raw bits: the height XOR 0x80000000.

### 5.3 Holes

When the tile's entry has bit 0 of flags, each node has a validity bit first, coded
with a context: the level, the kind (side or line, centre, edge) and how many of the
node's predictors are valid. A corner's bit has a context of its own. A hole has no
more bits. Its height is its prediction (a corner: 0), so the pyramid stays whole. A
tile with no holes codes no validity bits.

### 5.4 The coder

The coder is LZMA's binary range coder with 15-bit probabilities. Each probability
starts at 1/2 and moves by 1/32 of the distance after each bit. See `rc.hpp`.

## 6. The method

The u32 `method` in the header gives the stream framing:

| Value | Framing |
|---:|---|
| 0 | LZMA framing. The streams are byte for byte those of mundosm's `heightmap.hpp` (format 3). For tests and comparison. |
| 1 | Trimmed framing (pyre v0.1). The first byte (always 0) is not written. The flush writes the shortest value in the final interval. Trailing zero bytes are cut. The decoder reads zeros past the end. |

With method 1 a decoder cannot see a cut stream by itself. The tile's CRC finds it.

## 7. The checks of a reader

`Reader::open` does all these checks once. After them, every access is inside the
file:

- the magic, the version, the flags and the method;
- headerBytes, the grid count and the header's CRC;
- the parameters (section 3.1);
- each grid's text is inside the header;
- the index is at indexOffset, has the right size, ends at the end of the file, and has
  the right CRC; fileBytes is the size of the file;
- each entry: its grid exists; the entries are in order with no two the same; the tile
  is between the header and the index; headBytes ≤ totalBytes; the block ends go up
  and the last is totalBytes; the tile's epsilon is from 0 to 2^29.

The tile CRCs are not checked at open, so that a reader does not read every tile.
Call `Reader::verifyTile` before a tile is used. The decoder is safe without it (it
never reads outside a stream), but damaged bytes give wrong heights.

## 8. Versions

Version 1 is this document. A change that an old reader cannot read gets a new
version number. A reader refuses a version that it does not know, with a message that
gives both versions.
