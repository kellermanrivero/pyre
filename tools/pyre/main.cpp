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
  pyre decode <file.pyre> -o <out.tif> [--bbox …] [--level STEP] [--grid K] [--nodata V] [--json]
  pyre verify <file.pyre> <sources…> [--threads N] [--json]
  pyre bench <sources…> [--epsilon …] [--unit …] [--tile …] [--block …] [--base …]
             [--block-levels …] [--threads N] [--reference] [--json]

  --epsilon  the error limit in vertical units (metres); 0 is lossless
  --unit     the height quantum q; the error against the float source is at most ε + q/2
  --level    the pyramid step to decode, in cells: 1 (all), 2, 4 … up to the base step
  --bbox     a rectangle in the CRS of the sources
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
    for (int k = 2; k < argc; ++k) {
        std::string s = argv[k];
        if (s == "-o") s = "--output";
        if (s.size() > 2 && s.compare(0, 2, "--") == 0) {
            const std::string name = s.substr(2);
            if (takesValue(name)) {
                if (k + 1 >= argc) {
                    std::fprintf(stderr, "pyre: --%s needs a value\n", name.c_str());
                    return 2;
                }
                a.options[name] = argv[++k];
            } else {
                a.options[name] = "1";
            }
        } else {
            a.positional.push_back(s);
        }
    }
    try {
        if (command == "info") return cmdInfo(a);
        if (command == "encode") return cmdEncode(a);
        if (command == "decode") return cmdDecode(a);
        if (command == "verify") return cmdVerify(a);
        if (command == "bench") return cmdBench(a);
        std::fprintf(stderr, "pyre: unknown command '%s'\n%s", command.c_str(), kUsage);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "pyre %s: %s\n", command.c_str(), e.what());
        return 1;
    }
}
