// Windows Installer packages (.msi), such as those built with the WiX Toolset.
//
// An MSI is a compound file whose streams hold a small relational database and
// the cabinets with the files. 7-Zip reads both containers but knows nothing
// about the tables, so on its own it can only give every file its internal key
// as a name, all in one flat folder. This module decodes the tables that say
// where files go:
//   File       key, component, "short|long" file name, size, install sequence
//   Component  component, directory
//   Directory  directory, parent, "target:source" names
//   Media      the cabinets, and up to which install sequence number each one's files go
// The storage of tables and strings is described by Wine's implementation
// (dlls/msi/table.c and string.c).
#include "report.hpp"
#include "sevenzip.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include <boost/nowide/utf/convert.hpp>
#include <boost/nowide/utf/utf.hpp>

namespace {

using bit7z::BitArchiveReader;
namespace BitFormat = bit7z::BitFormat;

constexpr uint16_t kValid = 0x0100, kString = 0x0800, kNullable = 0x1000;  // column type bits

// Text is stored in the database's code page: UTF-8 or, in practice,
// Windows-1252. Any other code page is read as Windows-1252.
std::string decode(uint32_t codepage, View bytes) {
    std::string text;
    if (codepage == 65001) {  // already UTF-8; damaged sequences become U+FFFD
        const std::string_view utf8 = chars(bytes);
        return nowide::utf::convert_string<char>(utf8.data(), utf8.data() + utf8.size());
    }
    static constexpr char16_t high[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                          0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                                          0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                          0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    for (const uint8_t byte : bytes)
        nowide::utf::utf_traits<char>::encode(
            static_cast<nowide::utf::code_point>(byte >= 0x80 && byte < 0xA0 ? high[byte - 0x80] : byte),
            std::back_inserter(text));
    return text;
}

struct Column {
    std::string name;
    uint16_t type = 0;
};

struct Table {
    std::vector<Column> columns;               // empty: the database has no such table
    std::vector<std::vector<uint32_t>> rows;   // string ids, or integers with the storage offset removed
    size_t column(std::string_view name) const {
        for (size_t i = 0; i < columns.size(); ++i)
            if (columns[i].name == name) return i;
        throw Error("MSI table lacks the column " + std::string(name));
    }
};

class Database {
public:
    explicit Database(const BitArchiveReader& file) : file_(file) {
        for (const auto& item : file) streams_[item.path()] = item.index();
        const Bytes pool = stream("!_StringPool"), data = stream("!_StringData");
        if (pool.size() < 4) throw Error("not a Windows Installer database (it has no string pool)");

        // _StringPool: a header, then (length, reference count) per string; the text is in _StringData.
        const auto header = pod<uint32_t>(pool);
        codepage = header & 0x7FFFFFFF;
        ref_size_ = header & 0x80000000 ? 3 : 2;  // more than 65535 strings: 3-byte references
        strings_.emplace_back();                  // id 0 is the null string
        size_t offset = 0;
        for (size_t entry = 1; entry * 4 + 4 <= pool.size(); ++entry) {
            uint32_t length = pod<uint16_t>(View(pool).subspan(entry * 4));
            const auto references = pod<uint16_t>(View(pool).subspan(entry * 4 + 2));
            if (length == 0 && references != 0 && entry * 4 + 8 <= pool.size())
                length = pod<uint32_t>(View(pool).subspan(++entry * 4));  // 64 KiB or more: the next entry is the length
            if (length > data.size() - offset) throw Error("corrupt MSI string pool");
            strings_.push_back(decode(codepage, View(data).subspan(offset, length)));
            offset += length;
        }

        // _Columns: table, number, name, type. Like every table it is stored column by column.
        const Bytes columns = stream("!_Columns");
        const size_t count = columns.size() / (2 * ref_size_ + 4);
        for (size_t row = 0; row < count; ++row) {
            const std::string& table = text(reference(columns, row * ref_size_));
            const size_t number = pod<uint16_t>(View(columns).subspan(count * ref_size_ + row * 2)) ^ 0x8000u;
            const std::string& name = text(reference(columns, count * (ref_size_ + 2) + row * ref_size_));
            const auto type = static_cast<uint16_t>(
                pod<uint16_t>(View(columns).subspan(count * (2 * ref_size_ + 2) + row * 2)) ^ 0x8000u);
            if (number == 0 || number > 255) throw Error("corrupt MSI column table");
            auto& list = schema_[table];
            if (list.size() < number) list.resize(number);
            list[number - 1] = {name, type};
        }
    }

    Table table(const std::string& name) const {
        Table result;
        const auto found = schema_.find(name);
        if (found == schema_.end()) return result;
        result.columns = found->second;
        const Bytes data = stream("!" + name);
        std::vector<size_t> widths;
        size_t row_width = 0;
        for (const Column& column : result.columns) {
            const bool is_stream = (column.type & ~kNullable) == (kString | kValid);
            widths.push_back(is_stream ? 2 : column.type & kString ? ref_size_ : (column.type & 0xFF) <= 2 ? 2 : 4);
            row_width += widths.back();
        }
        if (row_width == 0 || data.size() % row_width != 0) throw Error("unexpected size of the MSI table " + name);
        const size_t rows = data.size() / row_width;
        result.rows.assign(rows, std::vector<uint32_t>(widths.size()));
        size_t base = 0;
        for (size_t c = 0; c < widths.size(); ++c) {
            const bool is_text = result.columns[c].type & kString;
            for (size_t r = 0; r < rows; ++r) {
                uint32_t value = widths[c] == 4 ? pod<uint32_t>(View(data).subspan(base + r * 4))
                                                : reference(data, base + r * widths[c], widths[c]);
                // Integers are stored with an offset so that 0 can mean null.
                if (!is_text && value != 0) value -= widths[c] == 4 ? 0x80000000u : 0x8000u;
                result.rows[r][c] = value;
            }
            base += rows * widths[c];
        }
        return result;
    }

    const std::string& text(uint32_t id) const {
        static const std::string none;
        return id < strings_.size() ? strings_[id] : none;
    }

    std::optional<uint32_t> stream_index(std::string_view name) const {
        for (const auto& [stream, index] : streams_)
            if (std::ranges::equal(stream, name, [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }))
                return index;
        return std::nullopt;
    }

    uint32_t codepage = 0;

private:
    Bytes stream(const std::string& name) const {
        Bytes data;
        if (const auto found = streams_.find(name); found != streams_.end()) file_.extractTo(data, found->second);
        return data;
    }
    uint32_t reference(View data, size_t at) const { return reference(data, at, ref_size_); }
    static uint32_t reference(View data, size_t at, size_t width) {
        if (at + width > data.size()) throw Error("corrupt MSI table");
        return data[at] | data[at + 1] << 8 | (width == 3 ? data[at + 2] << 16 : 0);
    }

    const BitArchiveReader& file_;
    std::map<std::string, uint32_t> streams_;  // 7-Zip decodes the stream names: tables start with "!"
    std::vector<std::string> strings_;
    size_t ref_size_ = 2;
    std::map<std::string, std::vector<Column>> schema_;
};

std::string after(std::string_view text, char separator) {  // "short|long" -> "long"
    const size_t at = text.find(separator);
    return std::string(at == text.npos ? text : text.substr(at + 1));
}

struct Directory {
    std::string parent, name;  // name: the long target name from DefaultDir
};

// Where a directory's files go, relative to the output folder. The root
// (TARGETDIR) adds nothing. A "." name means "same as the parent"; directly
// under the root those are the standard folders, e.g. ProgramFiles64Folder, and
// keep their identifier as a folder name so that files from different standard
// folders stay apart.
fs::path folder_of(const std::map<std::string, Directory>& directories, const std::string& id, int depth = 0) {
    const auto found = directories.find(id);
    if (found == directories.end() || depth > 64) return {};
    const Directory& directory = found->second;
    if (directory.parent.empty() || directory.parent == id) return {};
    const fs::path base = folder_of(directories, directory.parent, depth + 1);
    if (directory.name != ".") return base / safe_relpath(directory.name);
    const auto parent = directories.find(directory.parent);
    const bool below_root = parent == directories.end() || parent->second.parent.empty() ||
                            parent->second.parent == directory.parent;
    return below_root ? base / safe_relpath(id) : base;
}

struct Entry {
    std::string key;  // File table key; also the file's name inside the cabinet
    fs::path path;
    uint64_t size = 0;
    uint32_t sequence = 0;
    // Where its data is: in no cabinet, in a cabinet that is there, or in one that is not.
    enum { loose, packed, lost } where = loose;
};

struct Medium {  // a row of the Media table
    std::string cabinet;         // "#name": a stream of the package; another name: a file next to it; empty: none
    uint32_t last_sequence = 0;  // its files have sequence numbers up to this one, after those of the medium before
    bool there = true;           // the cabinet could be opened
};

}  // namespace

namespace msi {

bool detect(const fs::path& file) {
    constexpr std::string_view magic{"\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8};  // compound file
    return fs::file_size(file) >= 8 && chars(read_range(file, 0, 8)) == magic;
}

void run(const Options& options) {
    const bit7z::Bit7zLibrary& engine = sevenzip(options);
    const BitArchiveReader file{engine, shown(options.input), BitFormat::Compound};
    const Database database(file);
    const auto text = [&](const std::vector<uint32_t>& row, size_t column) -> const std::string& {
        return database.text(row[column]);
    };

    std::map<std::string, std::string> properties;
    if (const Table table = database.table("Property"); !table.columns.empty())
        for (const auto& row : table.rows) properties[text(row, table.column("Property"))] = text(row, table.column("Value"));

    std::map<std::string, Directory> directories;
    if (const Table table = database.table("Directory"); !table.columns.empty())
        for (const auto& row : table.rows) {
            const std::string& names = text(row, table.column("DefaultDir"));  // "target:source", each "short|long"
            directories[text(row, table.column("Directory"))] = {text(row, table.column("Directory_Parent")),
                                                                 after(names.substr(0, names.find(':')), '|')};
        }

    std::map<std::string, std::string> component_directory;
    if (const Table table = database.table("Component"); !table.columns.empty())
        for (const auto& row : table.rows)
            component_directory[text(row, table.column("Component"))] = text(row, table.column("Directory_"));

    std::vector<Entry> entries;
    if (const Table table = database.table("File"); !table.columns.empty()) {
        const size_t key = table.column("File"), component = table.column("Component_"), name = table.column("FileName"),
                     size = table.column("FileSize"), sequence = table.column("Sequence");
        for (const auto& row : table.rows)
            entries.push_back({text(row, key),
                               folder_of(directories, component_directory[text(row, component)]) /
                                   safe_relpath(after(text(row, name), '|')),
                               row[size], row[sequence]});
    }
    std::ranges::sort(entries, {}, &Entry::sequence);
    OutputNames names;  // the same path twice gets ",2", ",3", ...
    for (size_t i = 0; i < entries.size(); ++i) entries[i].path = names.claim(entries[i].path, i);

    std::vector<Medium> media;  // in the order of the install sequence
    uint64_t cabinets = 0;
    if (const Table table = database.table("Media"); !table.columns.empty())
        for (const auto& row : table.rows) {
            media.push_back({text(row, table.column("Cabinet")), row[table.column("LastSequence")]});
            cabinets += !media.back().cabinet.empty();
        }
    std::ranges::stable_sort(media, {}, &Medium::last_sequence);

    // Opens a cabinet: a stream of the package itself or a file next to it, which need not be there
    // (null). Only one is open at a time, because the streams of a package share the package's file.
    const auto open = [&](const std::string& cabinet) -> std::unique_ptr<BitArchiveReader> {
        if (cabinet.starts_with('#')) {
            const auto index = database.stream_index(std::string_view(cabinet).substr(1));
            if (!index) throw Error("the package lacks its cabinet stream " + cabinet);
            return std::make_unique<BitArchiveReader>(engine, file, *index, BitFormat::Cab);
        }
        const fs::path external = options.input.parent_path() / safe_relpath(cabinet);
        if (!fs::exists(external)) return nullptr;
        return std::make_unique<BitArchiveReader>(engine, shown(external), BitFormat::Cab);
    };

    // First what the cabinets hold, so that the report can count the files that are in the package
    // and name what is not: a cabinet that is not there, with the files of its part of the install
    // sequence, and the files that are in no cabinet at all.
    std::map<std::string, Entry*> by_key;
    for (Entry& entry : entries) by_key[entry.key] = &entry;
    for (Medium& medium : media) {
        if (medium.cabinet.empty()) continue;
        const auto archive = open(medium.cabinet);
        medium.there = archive != nullptr;
        if (!archive) continue;
        for (const auto& item : *archive) {
            const auto found = by_key.find(item.path());
            if (found == by_key.end() || found->second->where == Entry::packed) continue;
            if (item.size() != found->second->size)
                throw Error("the size of " + item.path() + " differs from the File table");
            found->second->where = Entry::packed;
        }
    }
    std::vector<std::string> absent;  // what the package names and does not hold, for the report
    uint32_t before = 0;              // the last sequence number of the medium before
    for (const Medium& medium : media) {
        const uint32_t above = std::exchange(before, medium.last_sequence);
        if (medium.there) continue;
        uint64_t count = 0;
        for (Entry& entry : entries)
            if (entry.where == Entry::loose && entry.sequence > above && entry.sequence <= medium.last_sequence)
                entry.where = Entry::lost, ++count;
        absent.push_back("Cabinet " + printable(medium.cabinet) + " with " + amount(count, "file") +
                         " (a separate file, not found next to the package)");
    }
    uint64_t inside = 0, bytes = 0, loose = 0;
    for (const Entry& entry : entries) {
        if (entry.where == Entry::packed) ++inside, bytes += entry.size;
        if (entry.where == Entry::loose && ++loose <= 10)
            absent.push_back(printable(shown(entry.path)) + " (in no cabinet: an external or uncompressed file)");
    }
    if (loose > 10) absent.push_back("... and " + std::to_string(loose - 10) + " more");

    info(options) << heading(properties["ProductName"] + ' ' + properties["ProductVersion"], kind_name(Format::msi))
                  << '\n';
    detail(options) << "Code page " << database.codepage << '\n' << amount(cabinets, "cabinet") << '\n';
    info(options) << contents(options, inside, "file", bytes)
                  << (inside != entries.size() ? "; " + std::to_string(entries.size() - inside) + " not embedded" : "")
                  << '\n';
    info(options) << listed("Not embedded:", absent);
    if (options.list) {
        for (const Entry& entry : entries)
            if (entry.where == Entry::packed)
                nowide::cout << padded(entry.size) << "  " << printable(shown(entry.path)) << '\n';
        return;
    }
    info(options) << std::flush;  // the report so far is to be seen while the work goes on

    for (const Medium& medium : media) {
        if (medium.cabinet.empty() || !medium.there) continue;
        const auto archive = open(medium.cabinet);
        if (!archive) throw Error("the cabinet " + medium.cabinet + " is no longer there");
        if (options.test)
            archive->test();
        else
            archive->extractTo(shown(options.outdir), [&](const bit7z::BitArchiveItem& item) {
                const auto found = by_key.find(item.path());
                return found == by_key.end() ? std::string() : shown(found->second->path);
            });
    }
    info(options) << outcome(options, inside, inside, "file", options.test ? "no errors found" : "") << '\n';
}

}  // namespace msi
