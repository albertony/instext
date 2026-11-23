// What is printed: the report on an installer, and how its lines are made.
//
// The report goes to info(), which says nothing with --quiet, and its technical details go to
// detail(), which speaks only with --verbose. The list of -l is the one thing that is printed in
// any case. Errors go to the error stream, where they are thrown to.
//
// Every line starts at the left margin, except the items of a list: those are indented by two
// spaces below a line that ends in a colon, as in "Install chain:".
#pragma once

#include "common.hpp"

std::ostream& info(const Options& options);
std::ostream& detail(const Options& options);

// Text that comes from the installer, made fit to print: control characters, the characters that
// change the direction of text and bytes that are no UTF-8 become "?". A name can then neither
// move the cursor or change colours in a terminal nor reorder what its line shows.
std::string printable(std::string_view text);
std::string padded(uint64_t size);                          // right-aligned in 12 columns, as in the listings
std::string amount(uint64_t count, std::string_view unit);  // "1 file", "3 files"
// A list: its header, which is a line that ends in a colon, and below it one indented line for
// every item. Without items there is no list: the result is empty.
std::string listed(std::string_view header, const std::vector<std::string>& items);
// A folder or file of the output for a message. It is shown through the name the command line gives
// the output folder: "out/_internals" after "-o out", whatever the current folder is.
std::string place(const Options& options, const fs::path& path);

// The lines of a report, in their order. With --verbose it starts by naming the file it is about:
// "Reading <file>".
std::string reading(const Options& options);
// What a file of this format is called: "Windows Installer package". This is the one place where
// those names are written.
std::string_view kind_name(Format format);
// The first line otherwise: "<product> - <kind>", or the kind alone where no product is known.
std::string heading(std::string_view product, std::string_view kind);
// What the file holds: "3411 files, 722.5 MiB", or "No files". The size is in binary units, as
// Windows counts; --verbose adds it to the byte: "722.5 MiB (757580680 bytes)".
std::string contents(const Options& options, uint64_t count, std::string_view unit, uint64_t bytes);
// The last line: "Tested: <verified>" or "Extracted to <folder>; <verified>". When not everything
// was there to be had, it says how much: "Extracted 240 of 244 files to <folder>". And when there
// was nothing: "Nothing to extract".
std::string outcome(const Options& options, uint64_t done, uint64_t all, std::string_view unit,
                    std::string_view verified);
// And what -i adds below it: "Wrote 7 internal files to <folder>".
std::string wrote_internals(const Options& options, uint64_t count);
