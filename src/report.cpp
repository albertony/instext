#include "report.hpp"

#include <boost/nowide/utf/utf.hpp>

std::string printable(std::string_view text) {
    std::string out;
    for (auto at = text.begin(); at != text.end();) {
        const auto start = at;
        const nowide::utf::code_point c = nowide::utf::utf_traits<char>::decode(at, text.end());
        const bool broken = c == nowide::utf::illegal || c == nowide::utf::incomplete;
        if (broken) at = start + 1;  // one byte that is no UTF-8; go on right behind it
        // C0 and C1 controls with DEL; line and paragraph separator and the embeddings and overrides
        // (U+2028 to U+202E); the isolates (U+2066 to U+2069).
        const bool control = c < 0x20 || (c >= 0x7F && c < 0xA0), direction = c >= 0x2066 && c <= 0x2069;
        if (broken || control || direction || (c >= 0x2028 && c <= 0x202E))
            out += '?';
        else
            out.append(start, at);
    }
    return out;
}

namespace {
std::ostream silent(nullptr);  // a stream without a buffer takes everything and prints nothing
}

std::ostream& info(const Options& options) {
    if (options.quiet) return silent;
    return nowide::cout;
}

std::ostream& detail(const Options& options) {
    if (!options.verbose) return silent;
    return nowide::cout;
}

std::string amount(uint64_t count, std::string_view unit) {
    return std::to_string(count) + ' ' + std::string(unit) + (count == 1 ? "" : "s");
}

std::string listed(std::string_view header, const std::vector<std::string>& items) {
    if (items.empty()) return {};
    std::string text = std::string(header) + '\n';
    for (const std::string& item : items) text += "  " + item + '\n';
    return text;
}

namespace {
// "722.5 MiB"; below 1 KiB the bytes themselves, which is why the return value says whether it rounded.
std::pair<std::string, bool> rounded(uint64_t bytes) {
    constexpr std::pair<uint64_t, std::string_view> units[] = {{1 << 30, "GiB"}, {1 << 20, "MiB"}, {1 << 10, "KiB"}};
    for (size_t unit = 0; unit < std::size(units); ++unit) {
        const auto& [size, name] = units[unit];
        if (bytes < size) continue;
        uint64_t whole = bytes / size, tenth = (bytes % size * 10 + size / 2) / size;
        if (tenth == 10) ++whole, tenth = 0;
        if (whole == 1024 && unit != 0) return {"1.0 " + std::string(units[unit - 1].second), true};  // rounded up
        return {std::to_string(whole) + '.' + std::to_string(tenth) + ' ' + std::string(name), true};
    }
    return {amount(bytes, "byte"), false};
}
}  // namespace

std::string contents(const Options& options, uint64_t count, std::string_view unit, uint64_t bytes) {
    if (count == 0) return "No " + std::string(unit) + 's';
    const auto [size, is_rounded] = rounded(bytes);
    const std::string exact = options.verbose && is_rounded ? " (" + amount(bytes, "byte") + ')' : "";
    return amount(count, unit) + ", " + size + exact;
}

std::string place(const Options& options, const fs::path& path) {
    const fs::path below = path.lexically_relative(options.root);
    fs::path named = path;  // what lies elsewhere is shown in full
    if (below == ".")
        named = options.named;
    else if (!below.empty() && *below.begin() != "..")
        named = options.named / below;
    return printable(shown(named.make_preferred()));
}

std::string reading(const Options& options) { return "Reading " + place(options, options.input); }

std::string_view kind_name(Format format) {
    switch (format) {
        case Format::msi: return "Windows Installer package";
        case Format::burn: return "WiX Burn bundle";
        case Format::inno: return "Inno Setup installer";
        case Format::nsis: return "NSIS installer";
        case Format::sfx: return "7-Zip self-extracting archive";
        case Format::archive: return "Archive";
        case Format::unknown: break;  // no kind: such a file gets no report
    }
    return {};
}

std::string heading(std::string_view product, std::string_view kind) {
    std::string name = printable(product);
    name.erase(0, name.find_first_not_of(' '));  // name or version can be missing
    name.erase(name.find_last_not_of(' ') + 1);
    return name.empty() ? std::string(kind) : name + " - " + std::string(kind);
}

std::string outcome(const Options& options, uint64_t done, uint64_t all, std::string_view unit,
                    std::string_view verified) {
    if (all == 0) return options.test ? "Nothing to test" : "Nothing to extract";
    std::string text = options.test ? "Tested" : "Extracted";
    if (done != all) text += ' ' + std::to_string(done) + " of " + amount(all, unit);
    if (!options.test) text += " to " + place(options, options.outdir);
    if (!verified.empty()) text += (options.test && done == all ? ": " : "; ") + std::string(verified);
    return text;
}

std::string padded(uint64_t size) {
    std::string text = std::to_string(size);
    text.insert(0, text.size() < 12 ? 12 - text.size() : 0, ' ');
    return text;
}

std::string wrote_internals(const Options& options, uint64_t count) {
    return "Wrote " + amount(count, "internal file") + " to " + place(options, options.outdir / kInternals);
}
