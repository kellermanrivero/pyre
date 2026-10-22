// pyre encode <sources…> --layer-of <file.pyre> --kind K: the second pass. It
// adds a layer to a file that has its heights already. The file's tiles (and
// the layers it has) are copied byte for byte; the new layer's tile goes next
// to each tile. Without -o the file is replaced (through a new file and a rename).
//
// pyre verify <file.pyre> --layer L <sources…>: checks layer L against the
// sources again.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "commands.hpp"
#include "files.hpp"
#include "json.hpp"
#include "pyre_source.hpp"
#include "surface.hpp"

namespace pyre::tool {

namespace {

/// A tile of the file as an EncodedTile: its bytes, cut at its block ends.
template <class BlockOf>
EncodedTile rawTile(std::span<const std::uint8_t> bytes, const TileEntry& e, std::size_t blocks, BlockOf&& blockOf) {
    EncodedTile t;
    t.head.assign(bytes.begin(), bytes.begin() + e.headBytes);
    t.blocks.resize(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
        const auto s = blockOf(b);
        t.blocks[b].assign(s.begin(), s.end());
    }
    t.min = e.min;
    t.max = e.max;
    t.holes = (e.flags & kTileHoles) != 0;
    return t;
}

SurfaceOptions surfaceOptions(const LayerInfo& l) {
    SurfaceOptions o;
    o.unit = l.unit;
    o.min = l.param;
    o.stride = l.stride;
    return o;
}

/// One ground source a grid: the file's own heights, holes as the decoder's predictions.
std::vector<std::unique_ptr<PyreSource>> groundOf(const std::string& file, std::size_t grids) {
    std::vector<std::unique_ptr<PyreSource>> g;
    for (std::size_t k = 0; k < grids; ++k) g.push_back(PyreSource::open(file + "#" + std::to_string(k), 24, true));
    return g;
}

}  // namespace

int cmdAddLayer(const Args& a) {
    const std::string file = a.get("layer-of");
    if (a.positional.empty() || !a.has("kind"))
        throw std::runtime_error("usage: pyre encode <sources…> --layer-of <file.pyre> --kind K [-o <out.pyre>]");
    const std::uint32_t kind = a.count("kind", 0);
    if (kind != kKindAboveGround)
        throw std::runtime_error("--kind " + std::to_string(kind) + ": this tool makes kind 1 (the height above the ground) only");
    const std::string out = a.get("output", file);
    const unsigned threads = threadsOf(a);
    const auto t0 = std::chrono::steady_clock::now();

    MappedFile map(file);
    Reader r;
    if (const Status st = r.open(map.bytes()); st != Status::Ok) throw std::runtime_error(file + ": " + toString(st) + ": " + r.error());
    const FileInfo& old = r.info();
    for (const auto& l : old.layers)
        if (l.kind == kind) throw std::runtime_error(file + " has a layer of kind " + std::to_string(kind) + " already");

    LayerInfo l;
    l.kind = std::uint8_t(kind);
    l.unit = a.number("unit", 3);
    l.param = a.number("min", 3);
    l.stride = a.count("stride", 2);
    const double epsilon = a.number("epsilon", 0);
    if (!(l.unit > 0) || !std::isfinite(l.unit) || !(l.param >= 0) || !std::isfinite(l.param) || !(epsilon >= 0))
        throw std::runtime_error("--unit must be more than 0, --min and --epsilon 0 or more");
    l.d = std::int32_t(std::floor(epsilon / l.unit + 1e-9));
    l.method = old.method;
    if (!layerValid(old.params, l))
        throw std::runtime_error("--stride must be a power of 2 that divides the tile, the block and the base, and leaves a base "
                                 "step of at least 2^block-levels; --epsilon is too large");
    FileInfo info = old;
    info.layers.push_back(l);
    const std::size_t newL = info.layers.size() - 1;
    const Params p = old.params, lp = layerParams(p, l.stride);

    std::vector<NodePlaces> places;
    for (const auto& g : old.grids) places.emplace_back(g);
    const auto ground = groundOf(file, old.grids.size());
    const SheetSet surface(a.positional);
    const SurfaceMaker maker(surface, surfaceOptions(l));

    // The new layer's tile at each tile of the file, on many threads; added in index order.
    const std::size_t n = r.tiles();
    struct Done {
        std::optional<EncodedTile> tile;
        std::uint64_t nonZero = 0;
    };
    std::vector<Done> done(n);
    std::vector<char> ready(n, 0);
    std::mutex m;
    std::size_t next = 0, layerTiles = 0;
    std::uint64_t layerBytes = 0, layerNodes = 0, objectNodes = 0;
    Status addStatus = Status::Ok;
    const std::string tmp = out + ".part";
    FileSink sink(tmp);
    Builder b;
    need(b.begin(info, sink), "cannot start the file");

    parallelFor(n, threads, [&](std::size_t k) {
        const TileEntry e = r.entry(k);
        std::vector<std::int32_t> x;
        std::vector<std::uint8_t> valid;
        const std::size_t count = maker.make(*ground[e.grid], old.grids[e.grid], places[e.grid], p, e.ti, e.tj, x, valid);
        Done d;
        if (count > 0) {
            d.tile.emplace();
            const auto v = count == lp.nodes() ? std::span<const std::uint8_t>{} : std::span<const std::uint8_t>(valid);
            need(encodeTile(lp, l.method, l.d, x, v, *d.tile), "cannot encode a layer tile");
            for (std::size_t q = 0; q < x.size(); ++q) d.nonZero += valid[q] && x[q] > 0;
        }
        std::lock_guard<std::mutex> lock(m);
        done[k] = std::move(d);
        ready[k] = 1;
        while (next < n && ready[next] && addStatus == Status::Ok) {
            const TileEntry en = r.entry(next);
            const auto bytes = r.tileBytes(next);
            addStatus = b.add(en.grid, en.ti, en.tj, rawTile(bytes, en, p.blocks(), [&](std::size_t q) { return r.block(next, q); }), en.d);
            for (std::size_t L = 0; L < old.layers.size() && addStatus == Status::Ok; ++L)
                if (const auto lk = r.layerFind(L, en.grid, en.ti, en.tj)) {
                    const TileEntry le = r.layerEntry(L, *lk);
                    addStatus = b.addLayer(L, le.grid, le.ti, le.tj,
                                           rawTile(r.layerTileBytes(L, *lk), le, r.layerParams(L).blocks(),
                                                   [&](std::size_t q) { return r.layerBlock(L, *lk, q); }),
                                           le.d);
                }
            if (done[next].tile && addStatus == Status::Ok) {
                ++layerTiles;
                layerBytes += done[next].tile->bytes();
                layerNodes += done[next].tile->valid;
                objectNodes += done[next].nonZero;
                addStatus = b.addLayer(newL, en.grid, en.ti, en.tj, *done[next].tile, l.d);
            }
            done[next] = {};
            ++next;
        }
    });
    need(addStatus, "cannot write a tile");
    // The tiles of the old layers at places with no main tile.
    for (std::size_t L = 0; L < old.layers.size(); ++L)
        for (std::size_t k = 0; k < r.layerTiles(L); ++k) {
            const TileEntry le = r.layerEntry(L, k);
            if (r.find(le.grid, le.ti, le.tj)) continue;
            need(b.addLayer(L, le.grid, le.ti, le.tj,
                            rawTile(r.layerTileBytes(L, k), le, r.layerParams(L).blocks(), [&](std::size_t q) { return r.layerBlock(L, k, q); }),
                            le.d),
                 "cannot write a layer tile");
        }
    need(b.finish(), "cannot finish the file");
    if (!sink.close()) throw std::runtime_error("cannot write " + tmp);
    std::filesystem::rename(tmp, out);
    const std::uint64_t bytes = MappedFile(out).bytes().size();
    const double secs = seconds(t0, std::chrono::steady_clock::now());

    if (a.has("json")) {
        Json j;
        j.add("output", out).add("layer", unsigned(newL + 1)).add("kind", unsigned(kind)).add("stride", unsigned(l.stride));
        j.add("unit", l.unit).add("min", l.param).add("error_quanta", l.d).add("tiles", std::uint64_t(n));
        j.add("layer_tiles", std::uint64_t(layerTiles)).add("layer_bytes", layerBytes).add("layer_nodes", layerNodes);
        j.add("object_nodes", objectNodes).add("bytes", bytes).add("old_bytes", std::uint64_t(map.bytes().size()));
        j.add("bits_per_layer_node", layerNodes ? double(layerBytes) * 8 / double(layerNodes) : 0.0);
        j.add("seconds", secs).add("threads", threads);
        std::printf("%s\n", j.str().c_str());
    } else {
        std::printf("wrote %s: layer %zu (kind %u, every %u nodes, steps of %g, 0 under %g): %zu tiles, %llu bytes, %.4f bits a layer "
                    "node; %llu of %llu layer nodes above the ground; the file %llu → %llu bytes, %.1f s on %u threads\n",
                    out.c_str(), newL + 1, kind, l.stride, l.unit, l.param, layerTiles, (unsigned long long)layerBytes,
                    layerNodes ? double(layerBytes) * 8 / double(layerNodes) : 0.0, (unsigned long long)objectNodes,
                    (unsigned long long)layerNodes, (unsigned long long)map.bytes().size(), (unsigned long long)bytes, secs, threads);
    }
    return 0;
}

int verifyLayer(const Args& a) {
    if (a.positional.size() < 2) throw std::runtime_error("usage: pyre verify <file.pyre> --layer L <sources…>");
    const std::string file = a.positional[0];
    MappedFile map(file);
    Reader r;
    if (const Status st = r.open(map.bytes()); st != Status::Ok) throw std::runtime_error(file + ": " + toString(st) + ": " + r.error());
    const FileInfo& fi = r.info();
    const std::uint32_t layer = a.count("layer", 0);
    if (layer == 0 || layer > r.layers()) throw std::runtime_error("the file has " + std::to_string(r.layers()) + " layers; no layer " + a.get("layer"));
    const std::size_t L = layer - 1;
    const LayerInfo& l = fi.layers[L];
    if (l.kind != kKindAboveGround) throw std::runtime_error("layer " + a.get("layer") + " is of kind " + std::to_string(l.kind) + ", which verify does not know");
    const unsigned threads = threadsOf(a);
    const auto t0 = std::chrono::steady_clock::now();
    const Params p = fi.params, lp = r.layerParams(L);
    std::vector<NodePlaces> places;
    for (const auto& g : fi.grids) places.emplace_back(g);
    const auto ground = groundOf(file, fi.grids.size());
    const SheetSet surface(std::vector<std::string>(a.positional.begin() + 1, a.positional.end()));
    const SurfaceMaker maker(surface, surfaceOptions(l));

    std::mutex m;
    std::uint64_t checked = 0, breaches = 0, maskErrors = 0, missing = 0, damaged = 0;
    std::int64_t worst = 0;
    parallelFor(r.tiles(), threads, [&](std::size_t k) {
        const TileEntry e = r.entry(k);
        std::vector<std::int32_t> want, got(lp.nodes());
        std::vector<std::uint8_t> wantValid, mask;
        const std::size_t count = maker.make(*ground[e.grid], fi.grids[e.grid], places[e.grid], p, e.ti, e.tj, want, wantValid);
        const auto lk = r.layerFind(L, e.grid, e.ti, e.tj);
        std::uint64_t c = 0, br = 0, me = 0, mi = 0, dm = 0;
        std::int64_t w = 0;
        if (!lk) {
            mi = count > 0;
        } else if (!r.verifyLayerTile(L, *lk)) {
            dm = 1;
        } else {
            const TileEntry le = r.layerEntry(L, *lk);
            mask.assign((le.flags & kTileHoles) ? lp.nodes() : 0, 0);
            Scratch s;
            need(r.decodeLayerWindow(L, *lk, 0, 0, lp.tile, lp.tile, 1, got, mask, s), "cannot decode a layer tile");
            for (std::size_t q = 0; q < lp.nodes(); ++q) {
                const bool v = mask.empty() || mask[q];
                if (v != bool(wantValid[q])) {
                    ++me;
                    continue;
                }
                if (!v) continue;
                ++c;
                const std::int64_t err = std::abs(std::int64_t(got[q]) - want[q]);
                w = std::max(w, err);
                br += err > le.d;
            }
        }
        std::lock_guard<std::mutex> lock(m);
        checked += c;
        breaches += br;
        maskErrors += me;
        missing += mi;
        damaged += dm;
        worst = std::max(worst, w);
    });
    const bool ok = breaches == 0 && maskErrors == 0 && missing == 0 && damaged == 0;
    const double secs = seconds(t0, std::chrono::steady_clock::now());
    if (a.has("json")) {
        Json j;
        j.add("ok", ok).add("layer", unsigned(layer)).add("nodes_checked", checked).add("breaches", breaches).add("mask_errors", maskErrors);
        j.add("missing_tiles", missing).add("damaged_tiles", damaged).add("max_error_quanta", worst).add("unit", l.unit).add("seconds", secs);
        std::printf("%s\n", j.str().c_str());
    } else {
        std::printf("%s: layer %u, %llu nodes checked against the sources; max error %lld quanta of %g (the limit is %d)\n",
                    ok ? "ok" : "FAILED", layer, (unsigned long long)checked, (long long)worst, l.unit, l.d);
        if (!ok)
            std::printf("breaches %llu, mask errors %llu, missing tiles %llu, damaged tiles %llu\n", (unsigned long long)breaches,
                        (unsigned long long)maskErrors, (unsigned long long)missing, (unsigned long long)damaged);
    }
    return ok ? 0 : 1;
}

}  // namespace pyre::tool
