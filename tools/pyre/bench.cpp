// pyre bench: encode and decode sources in memory; size, speed, error.
//
// With PYRE_HAVE_REFERENCE (the build finds mundosm's heightmap.hpp, read
// only), --reference also encodes and decodes the same 1001 × 1001 windows
// with the reference: the tiles with no holes (the reference has no no-data).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <stdexcept>

#include "commands.hpp"
#include "grid.hpp"
#include "json.hpp"

#ifdef PYRE_HAVE_REFERENCE
#include "heightmap.hpp"
#endif

namespace pyre::tool {

namespace {

using Clock = std::chrono::steady_clock;

struct TileData {
    std::uint32_t ti, tj;
    std::vector<std::int32_t> x;
    std::vector<std::uint8_t> valid;   // empty: all valid
    std::uint64_t count;
    EncodedTile enc;
    std::vector<std::int32_t> recon;
};

struct Result {
    std::string name;
    std::size_t tiles = 0, tilesWithHoles = 0;
    std::uint64_t uniqueValid = 0, nodes = 0, fileBytes = 0, streamBytes = 0;
    double enc1 = 0, encN = 0, dec1 = 0, decN = 0;   // seconds
    double headUs = 0, blockUs = 0, blockUsMax = 0;
    std::int64_t maxErrQuanta = 0;
    double maxErr = 0, sumSq = 0;
    std::uint64_t errNodes = 0;
    bool decodeMatches = true;
    // The reference, on the tiles with no holes.
    std::size_t refTiles = 0;
    std::uint64_t refBytes = 0, pyreBytesSame = 0, refNodes = 0;
    double refDec = 0, pyreDecSame = 0, refEnc = 0, pyreEncSame = 0;
    std::int64_t refMaxErr = 0;
};

Params paramsOfArgs(const Args& a) {
    Params p;
    p.tile = a.count("tile", 1000);
    p.block = a.count("block", 200);
    p.base = a.count("base", 8);
    p.blockLevels = a.count("block-levels", 1);
    if (!p.valid()) throw std::runtime_error("bad parameters (see pyre encode)");
    return p;
}

void decodeTile(const Params& p, Method m, std::int32_t d, const TileData& t, std::vector<std::int32_t>& out,
                std::vector<std::uint8_t>& mask, Scratch& s) {
    const TileSpec spec{p, m, d, t.enc.holes};
    if (decodeHead(spec, t.enc.head, out, mask, 1, s) != Status::Ok) throw std::runtime_error("decode failed");
    const std::uint32_t kb = p.blocksPerSide();
    for (std::uint32_t by = 0; by < kb; ++by)
        for (std::uint32_t bx = 0; bx < kb; ++bx)
            if (decodeBlock(spec, t.enc.blocks[by * kb + bx], out, mask, bx, by, 1, s) != Status::Ok)
                throw std::runtime_error("decode failed");
}

Result benchSource(const Source& src, const Params& p, double unit, std::int32_t d, unsigned threads, bool reference,
                   std::uint32_t maxTiles) {
    Result r;
    r.name = baseName(src.info().path);
    const Method method = Method::v1();
    const GridInfo g = gridOf(src.info(), p);

    // Read every tile (not timed).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> places;
    for (std::uint32_t tj = 0; tj < g.tilesY; ++tj)
        for (std::uint32_t ti = 0; ti < g.tilesX; ++ti) places.push_back({ti, tj});
    std::vector<TileData> all(places.size());
    parallelFor(places.size(), threads, [&](std::size_t k) {
        TileData& t = all[k];
        t.ti = places[k].first;
        t.tj = places[k].second;
        std::vector<double> buf;
        t.count = readTile(src, p, unit, t.ti, t.tj, t.x, t.valid, buf);
        if (t.count == p.nodes()) t.valid.clear();
    });
    std::vector<TileData> tiles;
    for (auto& t : all)
        if (t.count > 0 && (maxTiles == 0 || tiles.size() < maxTiles)) tiles.push_back(std::move(t));
    all.clear();
    r.tiles = tiles.size();
    for (const auto& t : tiles) {
        r.tilesWithHoles += !t.valid.empty();
        r.nodes += p.nodes();
        const bool lastX = t.ti == g.tilesX - 1, lastY = t.tj == g.tilesY - 1;
        for (std::size_t j = 0; j < p.side(); ++j)
            for (std::size_t i = 0; i < p.side(); ++i)
                if ((i < p.tile || lastX) && (j < p.tile || lastY)) r.uniqueValid += t.valid.empty() || t.valid[j * p.side() + i];
    }
    if (tiles.empty()) return r;

    // Encode: one thread, then N.
    auto encodeAll = [&](unsigned n) {
        const auto t0 = Clock::now();
        parallelFor(tiles.size(), n, [&](std::size_t k) {
            TileData& t = tiles[k];
            t.recon.resize(p.nodes());
            if (encodeTile(p, method, d, t.x, t.valid, t.enc, t.recon) != Status::Ok) throw std::runtime_error("encode failed");
        });
        return seconds(t0, Clock::now());
    };
    r.enc1 = encodeAll(1);
    r.encN = encodeAll(threads);

    // The file, in memory.
    {
        FileInfo info;
        info.params = p;
        info.method = method;
        info.d = d;
        info.unit = unit;
        info.grids = {g};
        VectorSink sink;
        Builder b;
        b.begin(info, sink);
        for (const auto& t : tiles) {
            b.add(0, std::int32_t(t.ti), std::int32_t(t.tj), t.enc, d);
            r.streamBytes += t.enc.bytes();
        }
        b.finish();
        r.fileBytes = sink.bytes.size();
    }

    // Decode: one thread, then N.
    auto decodeAll = [&](unsigned n, bool check) {
        std::mutex m;
        const auto t0 = Clock::now();
        parallelFor(tiles.size(), n, [&](std::size_t k) {
            thread_local Scratch s;
            thread_local std::vector<std::int32_t> out;
            thread_local std::vector<std::uint8_t> mask;
            const TileData& t = tiles[k];
            out.resize(p.nodes());
            mask.resize(t.enc.holes ? p.nodes() : 0);
            decodeTile(p, method, d, t, out, mask, s);
            if (check && out != t.recon) {
                std::lock_guard<std::mutex> lock(m);
                r.decodeMatches = false;
            }
        });
        return seconds(t0, Clock::now());
    };
    decodeAll(threads, true);   // warm the caches; check
    r.dec1 = decodeAll(1, false);
    r.decN = decodeAll(threads, false);

    // One block: the head once, then each block alone (one thread).
    {
        Scratch s;
        std::vector<std::int32_t> out(p.nodes());
        std::vector<std::uint8_t> mask(p.nodes());
        double headSum = 0, blockSum = 0, blockMax = 0;
        std::uint64_t blocks = 0;
        const std::uint32_t kb = p.blocksPerSide();
        for (const auto& t : tiles) {
            const TileSpec spec{p, method, d, t.enc.holes};
            auto t0 = Clock::now();
            decodeHead(spec, t.enc.head, out, mask, 1, s);
            headSum += seconds(t0, Clock::now());
            for (std::uint32_t b = 0; b < p.blocks(); ++b) {
                t0 = Clock::now();
                decodeBlock(spec, t.enc.blocks[b], out, mask, b % kb, b / kb, 1, s);
                const double sec = seconds(t0, Clock::now());
                blockSum += sec;
                blockMax = std::max(blockMax, sec);
                ++blocks;
            }
        }
        r.headUs = headSum / double(tiles.size()) * 1e6;
        r.blockUs = blockSum / double(blocks) * 1e6;
        r.blockUsMax = blockMax * 1e6;
    }

    // The error: against the rounded source (the guarantee), and against the float source.
    {
        std::mutex m;
        parallelFor(tiles.size(), threads, [&](std::size_t k) {
            const TileData& t = tiles[k];
            std::vector<double> v;
            readTileValues(src, p, t.ti, t.tj, v);
            std::int64_t wq = 0;
            double w = 0, ss = 0;
            std::uint64_t c = 0;
            for (std::size_t n = 0; n < p.nodes(); ++n) {
                if (!t.valid.empty() && !t.valid[n]) continue;
                wq = std::max<std::int64_t>(wq, std::abs(std::int64_t(t.recon[n]) - t.x[n]));
                const double e = std::abs(double(t.recon[n]) * unit - v[n]);
                w = std::max(w, e);
                ss += e * e;
                ++c;
            }
            std::lock_guard<std::mutex> lock(m);
            r.maxErrQuanta = std::max(r.maxErrQuanta, wq);
            r.maxErr = std::max(r.maxErr, w);
            r.sumSq += ss;
            r.errNodes += c;
        });
    }

#ifdef PYRE_HAVE_REFERENCE
    if (reference) {
        namespace ref = mundosm::heightmap;
        if (p != Params{} ) throw std::runtime_error("--reference needs the reference's parameters (tile 1000, block 200, base 8, block levels 1)");
        std::vector<std::int32_t> out(p.nodes());
        std::vector<std::uint8_t> none;
        Scratch s;
        for (const auto& t : tiles) {
            if (!t.valid.empty()) continue;
            ++r.refTiles;
            r.refNodes += p.nodes();
            std::vector<std::int32_t> refRecon(p.nodes());
            auto t0 = Clock::now();
            const std::vector<std::uint8_t> bytes = ref::encode(t.x.data(), d, refRecon.data());
            r.refEnc += seconds(t0, Clock::now());
            t0 = Clock::now();
            EncodedTile e;
            encodeTile(p, method, d, t.x, {}, e);
            r.pyreEncSame += seconds(t0, Clock::now());
            // The reference's tile with its index entry; pyre's with its entry and block ends.
            r.refBytes += bytes.size() + sizeof(ref::Entry);
            r.pyreBytesSame += t.enc.bytes() + kEntryBytes + 4 * p.blocks();
            for (std::size_t n = 0; n < p.nodes(); ++n)
                r.refMaxErr = std::max<std::int64_t>(r.refMaxErr, std::abs(std::int64_t(refRecon[n]) - t.x[n]));
            // Decode, each twice; keep the faster (less noise).
            double bestRef = 1e30, bestPyre = 1e30;
            for (int rep = 0; rep < 2; ++rep) {
                t0 = Clock::now();
                if (!ref::decode(bytes.data(), bytes.size(), d, out.data(), 1)) throw std::runtime_error("reference decode failed");
                bestRef = std::min(bestRef, seconds(t0, Clock::now()));
                t0 = Clock::now();
                decodeTile(p, method, d, t, out, none, s);
                bestPyre = std::min(bestPyre, seconds(t0, Clock::now()));
            }
            r.refDec += bestRef;
            r.pyreDecSame += bestPyre;
        }
    }
#else
    if (reference) throw std::runtime_error("this build has no reference (mundosm heightmap.hpp was not found)");
#endif
    return r;
}

Json toJson(const Result& r, unsigned threads) {
    Json j;
    const double mn = double(r.nodes) / 1e6;
    j.add("source", r.name).add("tiles", std::uint64_t(r.tiles)).add("tiles_with_holes", std::uint64_t(r.tilesWithHoles));
    j.add("valid_heights", r.uniqueValid).add("file_bytes", r.fileBytes).add("stream_bytes", r.streamBytes);
    j.add("bits_per_height", r.uniqueValid ? double(r.fileBytes) * 8 / double(r.uniqueValid) : 0.0);
    j.add("encode_mnodes_s_1", mn / r.enc1).add("encode_mnodes_s_n", mn / r.encN);
    j.add("decode_mnodes_s_1", mn / r.dec1).add("decode_mnodes_s_n", mn / r.decN).add("threads", threads);
    j.add("head_decode_us", r.headUs).add("block_decode_us", r.blockUs).add("block_decode_us_max", r.blockUsMax);
    j.add("max_error_quanta", r.maxErrQuanta).add("max_error", r.maxErr);
    j.add("rms", r.errNodes ? std::sqrt(r.sumSq / double(r.errNodes)) : 0.0).add("decode_matches_encoder", r.decodeMatches);
    if (r.refTiles) {
        const double rn = double(r.refNodes) / 1e6;
        Json ref;
        ref.add("tiles", std::uint64_t(r.refTiles)).add("reference_bytes", r.refBytes).add("pyre_bytes", r.pyreBytesSame);
        ref.add("size_ratio", double(r.pyreBytesSame) / double(r.refBytes));
        ref.add("reference_decode_mnodes_s", rn / r.refDec).add("pyre_decode_mnodes_s", rn / r.pyreDecSame);
        ref.add("decode_time_ratio", r.pyreDecSame / r.refDec);
        ref.add("reference_encode_mnodes_s", rn / r.refEnc).add("pyre_encode_mnodes_s", rn / r.pyreEncSame);
        ref.add("reference_max_error_quanta", r.refMaxErr);
        j.add("reference", ref);
    }
    return j;
}

}  // namespace

int cmdBench(const Args& a) {
    if (a.positional.empty()) throw std::runtime_error("usage: pyre bench <sources…> [parameters]");
    const Params p = paramsOfArgs(a);
    const double unit = a.number("unit", 0.01);
    const std::int32_t d = errorQuanta(a.number("epsilon", 0.99), unit);
    const unsigned threads = threadsOf(a);
    const bool reference = a.has("reference");
    const std::uint32_t maxTiles = a.count("max-tiles", 0);

    std::vector<Json> rows;
    Result total;
    total.name = "total";
    for (const auto& path : a.positional) {
        const auto src = openSource(path);
        const Result r = benchSource(*src, p, unit, d, threads, reference, maxTiles);
        rows.push_back(toJson(r, threads));
        total.tiles += r.tiles;
        total.tilesWithHoles += r.tilesWithHoles;
        total.uniqueValid += r.uniqueValid;
        total.nodes += r.nodes;
        total.fileBytes += r.fileBytes;
        total.streamBytes += r.streamBytes;
        total.enc1 += r.enc1;
        total.encN += r.encN;
        total.dec1 += r.dec1;
        total.decN += r.decN;
        total.headUs += r.headUs * double(r.tiles);
        total.blockUs += r.blockUs * double(r.tiles);
        total.blockUsMax = std::max(total.blockUsMax, r.blockUsMax);
        total.maxErrQuanta = std::max(total.maxErrQuanta, r.maxErrQuanta);
        total.maxErr = std::max(total.maxErr, r.maxErr);
        total.sumSq += r.sumSq;
        total.errNodes += r.errNodes;
        total.decodeMatches = total.decodeMatches && r.decodeMatches;
        total.refTiles += r.refTiles;
        total.refBytes += r.refBytes;
        total.pyreBytesSame += r.pyreBytesSame;
        total.refNodes += r.refNodes;
        total.refDec += r.refDec;
        total.pyreDecSame += r.pyreDecSame;
        total.refEnc += r.refEnc;
        total.pyreEncSame += r.pyreEncSame;
        total.refMaxErr = std::max(total.refMaxErr, r.refMaxErr);
        if (!a.has("json")) {
            const Json& j = rows.back();
            (void)j;
            const double mn = double(r.nodes) / 1e6;
            std::printf("%s: %zu tiles (%zu with holes), %.4f bits a height, %llu bytes; encode %.1f / %.1f Mnodes/s, decode %.1f / %.1f Mnodes/s "
                        "(1 / %u threads); head %.0f µs, block %.0f µs; max error %lld quanta, %.4g against the float source, RMS %.4g%s\n",
                        r.name.c_str(), r.tiles, r.tilesWithHoles, r.uniqueValid ? double(r.fileBytes) * 8 / double(r.uniqueValid) : 0.0,
                        (unsigned long long)r.fileBytes, mn / r.enc1, mn / r.encN, mn / r.dec1, mn / r.decN, threads, r.headUs, r.blockUs,
                        (long long)r.maxErrQuanta, r.maxErr, r.errNodes ? std::sqrt(r.sumSq / double(r.errNodes)) : 0.0,
                        r.decodeMatches ? "" : "  DECODE MISMATCH");
            if (r.refTiles)
                std::printf("  reference on %zu whole tiles: %llu bytes vs pyre %llu (%.4f); decode %.1f vs pyre %.1f Mnodes/s (time ratio %.3f)\n",
                            r.refTiles, (unsigned long long)r.refBytes, (unsigned long long)r.pyreBytesSame,
                            double(r.pyreBytesSame) / double(r.refBytes), double(r.refNodes) / 1e6 / r.refDec,
                            double(r.refNodes) / 1e6 / r.pyreDecSame, r.pyreDecSame / r.refDec);
        }
    }
    if (total.tiles) {
        total.headUs /= double(total.tiles);
        total.blockUs /= double(total.tiles);
    }
    if (a.has("json")) {
        Json j;
        j.add("epsilon", d * unit).add("unit", unit).add("tile", unsigned(p.tile)).add("block", unsigned(p.block));
        j.add("base", unsigned(p.base)).add("block_levels", unsigned(p.blockLevels));
        j.add("sources", rows).add("total", toJson(total, threads));
        std::printf("%s\n", j.str().c_str());
    } else if (a.positional.size() > 1) {
        const double mn = double(total.nodes) / 1e6;
        std::printf("total: %zu tiles, %.4f bits a height, decode %.1f Mnodes/s on 1 thread, max error %lld quanta\n", total.tiles,
                    total.uniqueValid ? double(total.fileBytes) * 8 / double(total.uniqueValid) : 0.0, mn / total.dec1,
                    (long long)total.maxErrQuanta);
        if (total.refTiles)
            std::printf("  reference on %zu whole tiles: size ratio %.4f, decode time ratio %.3f\n", total.refTiles,
                        double(total.pyreBytesSame) / double(total.refBytes), total.pyreDecSame / total.refDec);
    }
    return total.decodeMatches ? 0 : 1;
}

}  // namespace pyre::tool
