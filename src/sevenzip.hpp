// Access to the 7-Zip engine (7z.dll / 7z.so) through bit7z. 7-Zip supplies the
// NSIS, cabinet and compound-file readers; this tool adds what it lacks.
#pragma once

#include "common.hpp"

#include <bit7z/bit7z.hpp>

// Loads the engine on first use. Looks at options.sevenzip, then next to this
// program, then in the usual installation folders.
const bit7z::Bit7zLibrary& sevenzip(const Options& options);

// For --version --verbose: the engine that is used. It is loaded to find that out.
struct EngineInfo {
    fs::path file;                // where it was loaded from; empty if none could be loaded
    std::string version;          // what it reports about itself, as "26.06"; empty before 7-Zip 23.01
    std::vector<fs::path> tried;  // without an engine: the files that were tried
};
EngineInfo engine_info(const Options& options);
