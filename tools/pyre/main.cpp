// pyre — the command-line tool.

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "commands.hpp"

namespace {

const char* kUsage = R"(pyre 0.1 — terrain heights within ε, everywhere; decode only what you need

usage:
  pyre info <file.pyre> [--tiles] [--json]
  pyre encode <sources…> -o <out.pyre> [--epsilon 0.99] [--unit 0.01] [--tile 1000]
              [--block 200] [--base 8] [--block-levels 1] [--bbox minX,minY,maxX,maxY]
              [--threads N] [--json]
  pyre encode <sources…> --layer-of <file.pyre> --kind 1 [--unit 3] [--min 3] [--stride 2]
              [--epsilon 0] [-o <out.pyre>] [--threads N] [--json]
  pyre decode <file.pyre> -o <out.tif> [--bbox …] [--level STEP] [--grid K] [--layer L] [--nodata V] [--json]
  pyre verify <file.pyre> <sources…> [--layer L] [--threads N] [--json]
  pyre bench <sources…> [--epsilon …] [--unit …] [--tile …] [--block …] [--base …]
             [--block-levels …] [--threads N] [--reference] [--max-tiles N] [--json]
  pyre compare <file.pyre> <pack.alt> [--grid K] [--threads N] [--json]

  --epsilon  the error limit in vertical units (metres); 0 is lossless
  --unit     the height quantum q; the error against the float source is at most ε + q/2
  --level    the pyramid step to decode, in cells: 1 (all), 2, 4 … up to the base step
  --bbox     a rectangle in the CRS of the sources
  --layer-of the second pass: add a layer to a file that has its heights. Kind 1 is the
             height of objects above the ground: the sources are a surface model (e.g. the
             MDS02, rasters or folders), the ground is the file's own heights. Its values
             are in steps of --unit metres (0 under --min), the largest in each
             --stride × --stride nodes. Without -o the file is replaced.
  --layer    decode (or verify) layer L (1 …) of the file instead of the heights
  compare    a file made by pyre-resample against a reference heights pack (.alt)
             on the same local grid: the differences and the sizes
  --json     print one JSON object
)";

}  // namespace

int main(int argc, char** argv) {
    using namespace pyre::tool;
    if (argc < 2 || !std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "help")) {
        std::fputs(kUsage, argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    const std::string command = argv[1];
    Args a;
    try {
        a = parseArgs(argc, argv, 2, takesValue);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pyre: %s\n", e.what());
        return 2;
    }
    try {
        if (command == "info") return cmdInfo(a);
        if (command == "encode") return cmdEncode(a);
        if (command == "decode") return cmdDecode(a);
        if (command == "verify") return cmdVerify(a);
        if (command == "bench") return cmdBench(a);
        if (command == "compare") return cmdCompare(a);
        std::fprintf(stderr, "pyre: unknown command '%s'\n%s", command.c_str(), kUsage);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pyre %s: %s\n", command.c_str(), e.what());
        return 1;
    }
}
