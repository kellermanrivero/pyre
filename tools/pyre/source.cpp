// openSource: the reader for each file extension.

#include <cctype>

#include "pyre_source.hpp"
#include "source.hpp"
#include "tiff.hpp"

namespace pyre::tool {

std::string sourceExtension(const std::string& path) {
    std::string p = path;
    // "file.pyre#K" names grid K of a pyre file.
    if (const auto hash = p.rfind('#'); hash != std::string::npos && p.find_first_of("/\\", hash) == std::string::npos) p.resize(hash);
    std::string ext;
    if (const auto dot = p.find_last_of('.'); dot != std::string::npos && p.find_first_of("/\\", dot) == std::string::npos)
        ext = p.substr(dot + 1);
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::unique_ptr<Source> openSource(const std::string& path) {
    const std::string ext = sourceExtension(path);
    if (ext == "tif" || ext == "tiff") return TiffSource::open(path);
    if (ext == "pyre") return PyreSource::open(path);
    throw SourceError(path + ": unknown source format (expected .tif, .tiff or .pyre)");
}

}  // namespace pyre::tool
