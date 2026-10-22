#include "tile_epsilon.hpp"

#include <cmath>
#include <fstream>
#include <sstream>

#include "alt.hpp"
#include "grid.hpp"

namespace pyre::tool {

namespace {
void put(TileEpsilons& m, std::int32_t i, std::int32_t j, std::int32_t d) {
    const auto [it, added] = m.emplace(std::make_pair(i, j), d);
    if (!added && d < it->second) it->second = d;
}
}  // namespace

TileEpsilons parseTileEpsilons(const std::string& text, const LocalGrid& g, double tileM, double unit) {
    TileEpsilons m;
    std::istringstream in(text);
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream w(line);
        std::string kind;
        if (!(w >> kind)) continue;
        auto bad = [&](const std::string& why) { return SourceError("tile epsilons, line " + std::to_string(n) + ": " + why); };
        std::string rest;
        if (kind == "tile") {
            long long i = 0, j = 0;
            double eps = 0;
            if (!(w >> i >> j >> eps) || (w >> rest)) throw bad("give: tile <i> <j> <epsilon>");
            if (i < INT32_MIN || i > INT32_MAX || j < INT32_MIN || j > INT32_MAX) throw bad("the tile place is out of range");
            put(m, std::int32_t(i), std::int32_t(j), errorQuanta(eps, unit));
        } else if (kind == "box") {
            double lon0 = 0, lat0 = 0, lon1 = 0, lat1 = 0, eps = 0;
            if (!(w >> lon0 >> lat0 >> lon1 >> lat1 >> eps) || (w >> rest)) throw bad("give: box <minLon> <minLat> <maxLon> <maxLat> <epsilon>");
            if (!(lon0 <= lon1 && lat0 <= lat1)) throw bad("the box needs min ≤ max");
            const std::int32_t d = errorQuanta(eps, unit);
            const double i0 = std::floor(g.xOf(lon0) / tileM), i1 = std::floor(g.xOf(lon1) / tileM);
            const double j0 = std::floor(g.yOf(lat0) / tileM), j1 = std::floor(g.yOf(lat1) / tileM);
            if ((i1 - i0 + 1) * (j1 - j0 + 1) > 1e7) throw bad("the box holds too many tiles");
            for (double j = j0; j <= j1; ++j)
                for (double i = i0; i <= i1; ++i) put(m, std::int32_t(i), std::int32_t(j), d);
        } else {
            throw bad("a line starts with 'tile' or 'box', not '" + kind + "'");
        }
    }
    return m;
}

TileEpsilons readTileEpsilons(const std::string& path, const LocalGrid& g, double tileM, double unit) {
    std::ifstream f(path);
    if (!f) throw SourceError("cannot read " + path);
    std::ostringstream s;
    s << f.rdbuf();
    return parseTileEpsilons(s.str(), g, tileM, unit);
}

TileEpsilons tileEpsilonsOfPack(const std::string& path, double unit) {
    const AltPack pack(path);
    TileEpsilons m;
    for (std::size_t k = 0; k < pack.tiles(); ++k)
        if (pack.entry(k).errorCm != 0) put(m, pack.entry(k).i, pack.entry(k).j, errorQuanta(pack.errorOf(k) * 0.01, unit));
    return m;
}

void mergeTileEpsilons(TileEpsilons& a, const TileEpsilons& b) {
    for (const auto& [place, d] : b) put(a, place.first, place.second, d);
}

}  // namespace pyre::tool
