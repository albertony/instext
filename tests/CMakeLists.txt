# Tests: instext is run on real installers, and its output is compared with
# what is known about them.
#   ctest --test-dir <build folder> --output-on-failure
# The first test downloads the installers, about 75 MiB, see samples.cmake.

set(INSTEXT_TEST_SAMPLES "${CMAKE_CURRENT_SOURCE_DIR}/samples" CACHE PATH
    "Folder where the tests keep the installers they download")
set(samples "${INSTEXT_TEST_SAMPLES}")
set(work "${CMAKE_CURRENT_BINARY_DIR}/work")
file(MAKE_DIRECTORY "${work}")

add_test(NAME samples COMMAND ${CMAKE_COMMAND} "-Dfolder=${samples}" -P "${CMAKE_CURRENT_SOURCE_DIR}/samples.cmake")
set_tests_properties(samples PROPERTIES FIXTURES_SETUP samples TIMEOUT 3600)

# The engine for the tests: the one chosen for installing, or else whatever
# instext finds by itself. Inno Setup is tested with no engine in reach.
set(engine "")
if(INSTEXT_7ZIP_ENGINE)
    set(engine --7zip "${INSTEXT_7ZIP_ENGINE}")
endif()
set(no_engine --7zip "${work}/no-such-engine")

# check(<name> <regular expression that the output must match> <instext arguments>...)
function(check name expected)
    add_test(NAME ${name} COMMAND instext ${ARGN})
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected}")
    string(FIND "${ARGN}" "${samples}" names_sample)
    if(NOT names_sample EQUAL -1)  # such a test waits for the download
        set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED samples)
    endif()
endfunction()

# damaged(<name> <installer> <offset> <regular expression for the error> [cut])
# The test runs on a copy with one bit flipped in the byte at the offset or, with "cut", on a copy
# that ends there.
add_executable(damage damage.cpp)
target_link_libraries(damage PRIVATE Boost::nowide)
function(damaged name installer offset expected)
    add_test(NAME ${name}-copy COMMAND damage "${installer}" "${work}/${name}.bin" ${offset} ${ARGN})
    set_tests_properties(${name}-copy PROPERTIES FIXTURES_REQUIRED samples FIXTURES_SETUP ${name})
    add_test(NAME ${name} COMMAND instext ${engine} -t "${work}/${name}.bin")
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected}" FIXTURES_REQUIRED ${name})
endfunction()

set(inno "${samples}/innosetup-7.1.1-x64.exe")
check(inno-test    "136 files, 50\\.3 MiB.Tested: all SHA-256 hashes match" ${no_engine} -t "${inno}")
check(inno-list    "8876960  app.ISIDE\\.exe"                   ${no_engine} -l "${inno}")
check(inno-extract "Wrote 7 internal files to [^\n]*inno._internals\n"
      ${no_engine} --force -i -o "${work}/inno" "${inno}")
check(inno-6       "unsupported setup data format"              -l "${samples}/innosetup-6.7.4.exe")
damaged(inno-damaged "${inno}" 8600000 "does not decompress|SHA-256 mismatch")

# made(<name> <helper> <regular expression that the output must match> <arguments of the helper after its target>...)
# The test extracts a small installer that a helper writes first: makeinno an Inno Setup installer,
# makeburn a Burn bundle. Like every test here that extracts, it passes --force: on a second run of
# the tests the output folders are there, with the files of the first.
add_executable(makeinno makeinno.cpp)
target_link_libraries(makeinno PRIVATE Boost::hash2 Boost::nowide LibLZMA::LibLZMA ZLIB::ZLIB)
add_executable(makeburn makeburn.cpp)
target_link_libraries(makeburn PRIVATE Boost::hash2 Boost::nowide)
function(made name helper expected)
    add_test(NAME ${name}-make COMMAND ${helper} "${work}/${name}.exe" ${ARGN})
    set_tests_properties(${name}-make PROPERTIES FIXTURES_SETUP ${name})
    set(with ${engine})
    if(helper STREQUAL "makeinno")
        set(with ${no_engine})
    endif()
    add_test(NAME ${name} COMMAND instext ${with} --force -o "${work}/${name}" "${work}/${name}.exe")
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected}" FIXTURES_REQUIRED ${name})
endfunction()
# 200 files in one chunk, as with solid compression, every fifth of them empty: an empty file starts
# where the next one does, and the two have to be read in that order.
made(inno-empty-files makeinno "200 files, .*Extracted to .*all SHA-256 hashes match" 200 5)
# The name of that installer holds an escape sequence and a character that reverses the direction
# of text. Both are printed as "?", and no escape character is.
string(ASCII 27 escape)
check(inno-printable "Made-up \\?.31m\\?installer" ${no_engine} -l "${work}/inno-empty-files.exe")
set_tests_properties(inno-printable PROPERTIES FIXTURES_REQUIRED inno-empty-files
                     FAIL_REGULAR_EXPRESSION "${escape}")
# The same with 1 MiB of zeros behind the 200 file locations of 89 bytes each: the reader knows the
# size of that table and must not unpack whatever else its block holds.
made(inno-oversized makeinno "holds more than 17800 bytes" 200 5 1048576)
# Three files, each installed to 601 places: more than a program may have open at once on Windows.
# The last copy has to be the same as the first. The size in the report counts every copy, as the
# number of files does: 601 times the 203 bytes that are stored.
made(inno-copies makeinno "1803 files, 119\\.1 KiB.Extracted to .*all SHA-256 hashes match" 3 3 0 600)
set_tests_properties(inno-copies PROPERTIES FIXTURES_SETUP inno-copies-written)
add_test(NAME inno-copies-same COMMAND ${CMAKE_COMMAND} -E compare_files
         "${work}/inno-copies/app/file1.txt" "${work}/inno-copies/app/copy600/file1.txt")
set_tests_properties(inno-copies-same PROPERTIES FIXTURES_REQUIRED inno-copies-written)
# The helper dates the second file 2001-02-03 and the third a thousand years later. The first date
# has to be on the extracted file, here on one of its copies. The other is no time for a file, and
# the file must be left as it was written, which was just now.
add_test(NAME inno-time-kept COMMAND ${CMAKE_COMMAND} "-Dfile=${work}/inno-copies/app/copy600/file1.txt"
         -Dyear=2001 -P "${CMAKE_CURRENT_SOURCE_DIR}/modified.cmake")
add_test(NAME inno-time-absurd COMMAND ${CMAKE_COMMAND} "-Dfile=${work}/inno-copies/app/file2.txt"
         -Dyear=now -P "${CMAKE_CURRENT_SOURCE_DIR}/modified.cmake")
set_tests_properties(inno-time-kept inno-time-absurd PROPERTIES FIXTURES_REQUIRED inno-copies-written)
# Installers that claim to keep their file data in slice files: in a slice with a negative number,
# and in the last one that can be numbered, whose file is setup-2147483648.bin and is not there.
made(inno-slice-negative makeinno "slice numbers are corrupt" 3 3 0 0 -1)
made(inno-slice-last     makeinno "cannot open .*inno-slice-last-2147483648\\.bin" 3 3 0 0 2147483647)
# Three entries whose data is not in the installer: a file to copy from elsewhere, one to download
# and an archive to extract at install time. They are counted apart, and listed with the reason
# under the names they would have been written to, which have no braces.
set(skipped "  app.outside0\\.txt .external file, copied from outside the installer.\n")
set(skipped "${skipped}  app.outside1\\.txt .downloaded at install time.\n")
set(skipped "${skipped}  app.outside2\\.txt .archive extracted at install time.\n")
made(inno-outside makeinno "3 files, [^\n]*; 3 not embedded\nNot embedded:\n${skipped}Extracted to " 3 3 +3)

set(msi "${samples}/gh_2.105.0_windows_amd64.msi")
check(msi-test      "1 file, 40\\.0 MiB.Tested: no errors"  ${engine} -t "${msi}")
check(msi-list      "Program Files.GitHub CLI.gh\\.exe"  ${engine} -l "${msi}")
check(msi-no-engine "cannot load the 7-Zip engine"       ${no_engine} -l "${msi}")
damaged(msi-damaged "${msi}" 8000000 "Data error")

set(nsis "${samples}/Rainmeter-4.5.28.exe")
# The count depends on the engine: 7-Zip 26.06 lists one file more here, an empty uninst.exe, than 23.01
# and 26.00 do.
check(nsis-test     "10[23] files, .*Tested: no errors" ${engine} -t "${nsis}")
check(nsis-checksum "own checksum .CRC-32. matches" ${engine} -t "${nsis}")
check(nsis-names    "Rainmeter,2\\.exe"             ${engine} -l "${nsis}")
damaged(nsis-damaged "${nsis}" 1000000 "own checksum .CRC-32. does not match")

set(sfx "${samples}/7z2606-x64.exe")
check(sfx-test "107 files, .*Tested: no errors found" ${engine} -t "${sfx}")
damaged(sfx-damaged "${sfx}" 800000 "Data error|CRC failed")

set(burn "${samples}/wix314.exe")
set(absent "16 payloads, 30\\.6 MiB; 1 not embedded\nNot embedded:\n  redist[^\n]* .external: downloaded[^\n]*\n")
check(burn-test    "WiX Burn bundle\n${absent}Tested: all sizes and hashes match the manifest" ${engine} -t "${burn}")
# The output folder is named relative to the folder the test runs in here, and the reports have to
# show it by that name, for the bundle and for a package inside it.
check(burn-recurse "Extracted to burn; .*244 files, 28\\.3 MiB.Extracted to burn.data.core\\.msi\\.extracted\n"
      ${engine} --force -r -o burn "${burn}")
set_tests_properties(burn-recurse PROPERTIES WORKING_DIRECTORY "${work}" FIXTURES_SETUP burn-recursed)
# One of the bundle's packages, copied to a folder of its own without its cabinet, which is a
# separate file: the report counts what the package itself holds, which is nothing, and names the
# cabinet once, with the number of files that are in it.
file(MAKE_DIRECTORY "${work}/msi-alone")
add_test(NAME msi-alone-copy COMMAND ${CMAKE_COMMAND} -E copy "${work}/burn/data/core.msi" "${work}/msi-alone/core.msi")
set_tests_properties(msi-alone-copy PROPERTIES FIXTURES_REQUIRED burn-recursed FIXTURES_SETUP msi-alone)
set(alone "No files; 244 not embedded\nNot embedded:\n  Cabinet core1\\.cab with 244 files ")
set(alone "${alone}.a separate file, not found next to the package.\n")
check(msi-alone "Windows Installer package\n${alone}Nothing to test\n" ${engine} -t "${work}/msi-alone/core.msi")
set_tests_properties(msi-alone PROPERTIES FIXTURES_REQUIRED msi-alone)
damaged(burn-damaged "${burn}" 19500000 "Data error|does not match the size and hash")
damaged(burn-cut     "${burn}" 20000000 "the bundle is truncated" cut)
# Bundles made here. Payloads that share a name, also when only the case of letters differs, get
# ",2" and ",3" as in the other formats. And one with SHA-512 hashes, as WiX 4 and later write them.
made(burn-names makeburn "4 payloads, .*Extracted to .*all sizes and hashes match"
     sha1 readme.txt Readme.TXT "data\\a.bin" readme.txt)
check(burn-names-list "readme\\.txt.*Readme,2\\.TXT.*a\\.bin.*readme,3\\.txt" ${engine} -l "${work}/burn-names.exe")
set_tests_properties(burn-names-list PROPERTIES FIXTURES_REQUIRED burn-names)
made(burn-sha512 makeburn "2 payloads, .*Extracted to .*all sizes and hashes match" sha512 one.bin two.bin)
# Names that Windows takes for devices get "_" in front, whatever their case and extension, so that
# the data lands in a file. "console.txt" and "com10.txt" are no such names and stay as they are.
made(burn-devices makeburn "6 payloads, .*Extracted to .*all sizes and hashes match"
     sha1 NUL aux.txt Com1.log "sub\\LPT9" console.txt com10.txt)
set(devices "  _NUL\n.*  _aux\\.txt\n.*  _Com1\\.log\n.*  sub._LPT9\n.*  console\\.txt\n.*  com10\\.txt\n")
check(burn-devices-list "${devices}" ${engine} -l "${work}/burn-devices.exe")
set_tests_properties(burn-devices-list PROPERTIES FIXTURES_REQUIRED burn-devices)
# Payloads that are not in the bundle, with the reason in WiX's terms: external, which is a file
# of its own, with or without an address to fetch it from, or in a detached container, which is
# a cabinet file of its own. With -v such containers are counted next to the attached ones.
set(outside "  loose\\.bin .external: shipped next to the bundle.\n")
set(outside "${outside}  fetched\\.bin .external: downloaded or shipped next to the bundle.\n")
set(outside "${outside}  packed\\.bin .in the detached container made\\.cab.\n")
made(burn-outside makeburn "1 payload, [^\n]*; 3 not embedded\nNot embedded:\n${outside}Extracted to "
     sha1 in.bin external:loose.bin download:fetched.bin detached:packed.bin)
set(parts "\nBurn engine 3\\.x\nBootstrapper application with 1 internal file\n")
set(parts "${parts}1 attached container, 1 detached container\n")
set(chain "Install chain:\n  p0 .MsiPackage, 1 payload.\n  p1 .MsiPackage, 1 payload, not embedded.\n")
check(burn-outside-verbose "${parts}${chain}.*\n1 payload, [^\n]*; 3 not embedded\nNot embedded:\n"
      ${engine} -v -t "${work}/burn-outside.exe")
set_tests_properties(burn-outside-verbose PROPERTIES FIXTURES_REQUIRED burn-outside)
# A bundle with a package inside that cannot be read: with -r that is reported under the package's
# name, the rest is still extracted, and the run ends as a failure.
add_test(NAME burn-nested-make COMMAND makeburn "${work}/burn-nested.exe" sha1 fine.txt broken.msi)
set_tests_properties(burn-nested-make PROPERTIES FIXTURES_SETUP burn-nested)
check(burn-nested "error: broken\\.msi: .*error: 1 nested installer"
      ${engine} --force -r -o "${work}/burn-nested" "${work}/burn-nested.exe")
set_tests_properties(burn-nested PROPERTIES FIXTURES_REQUIRED burn-nested
                     FAIL_REGULAR_EXPRESSION "Reading |Looking for installers")
# With -v the run says what each payload turned out to be, and every report starts with the file it
# is about: the bundle first, then the package inside it.
set(found "among the 2 payloads:\n  fine\\.txt .nothing that InstExt reads.\n")
set(found "${found}  broken\\.msi .Windows Installer package.")
check(burn-nested-verbose
      "^Reading [^\n]*burn-nested\\.exe\n.*${found}\n\nReading [^\n]*burn-nested-v.broken\\.msi\n"
      ${engine} --force -v -r -o "${work}/burn-nested-v" "${work}/burn-nested.exe")
set_tests_properties(burn-nested-verbose PROPERTIES FIXTURES_REQUIRED burn-nested)

# The fallback for other archives, on archives made here from two files of this repository.
# pack(<mode> <archive> <format>): the files are named in the archive as they are given to
# "cmake -E tar", whatever folder CMake itself happens to run in.
function(pack mode archive format)
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar ${mode} "${work}/${archive}" --format=${format}
                            LICENSE.txt NOTICE.txt
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
endfunction()
pack(cf  texts.zip    zip)
pack(cf  texts.7z     7zip)
pack(czf texts.tar.gz gnutar)
# A self-extracting 7-Zip installer with settings for its stub, made by a helper from the 7z
# archive: with -v the settings are listed, and -i writes them as one of the installer's own files.
add_executable(makesfx makesfx.cpp)
target_link_libraries(makesfx PRIVATE Boost::nowide)
add_test(NAME sfx-settings-make COMMAND makesfx "${work}/sfx-settings.exe" "${work}/texts.7z")
set_tests_properties(sfx-settings-make PROPERTIES FIXTURES_SETUP sfx-settings)
set(settings "\nStub settings:\n  Title=.Made-up installer.\n  RunProgram=.setup\\.exe.\n2 files, [^\n]*\n")
set(settings "${settings}Extracted to [^\n]*\nWrote 1 internal file to [^\n]*_internals\n")
check(sfx-settings "^Reading [^\n]*\n7-Zip self-extracting archive\nHandled by [^\n]*${settings}"
      ${engine} --force -v -i -o "${work}/sfx-settings" "${work}/sfx-settings.exe")
set_tests_properties(sfx-settings PROPERTIES FIXTURES_REQUIRED sfx-settings)
check(archive-zip    "2 files, .*Tested: no errors found" ${engine} -t "${work}/texts.zip")
check(archive-7z     "2 files, .*Tested: no errors found" ${engine} -t "${work}/texts.7z")
check(archive-tar-gz "1 file, .*Tested: no errors found"  ${engine} -t "${work}/texts.tar.gz")
check(archive-none   "neither an installer" ${engine} -l "${CMAKE_CURRENT_LIST_FILE}")
# The folder of -i is this tool's own. What an archive or an installer holds under that name, in
# any case, is written under the name with ",2": here a folder of an archive, and a payload of a
# bundle that is named like the manifest, which -i writes there.
file(WRITE "${work}/reserved/_Internals/x.txt" "text\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E tar cf "${work}/reserved.zip" --format=zip _Internals
                WORKING_DIRECTORY "${work}/reserved" COMMAND_ERROR_IS_FATAL ANY)
check(archive-reserved "  _Internals,2.x\\.txt\n" ${engine} -l "${work}/reserved.zip")
add_test(NAME burn-reserved-make COMMAND makeburn "${work}/burn-reserved.exe" sha1 "_internals\\manifest.xml")
set_tests_properties(burn-reserved-make PROPERTIES FIXTURES_SETUP burn-reserved)
check(burn-reserved "Extracted to [^\n]*\nWrote 1 internal file to [^\n]*_internals\n"
      ${engine} --force -i -o "${work}/burn-reserved" "${work}/burn-reserved.exe")
check(burn-reserved-list "  _internals,2.manifest\\.xml\n" ${engine} -l "${work}/burn-reserved.exe")
set_tests_properties(burn-reserved burn-reserved-list PROPERTIES FIXTURES_REQUIRED burn-reserved)
# A device name in an archive gets its "_" too. Windows cannot make a file of such a name to pack,
# so the archive is made, and the test run, only elsewhere.
if(NOT WIN32)
    file(WRITE "${work}/devices/aux.txt" "text\n")
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar cf "${work}/devices.zip" --format=zip aux.txt
                    WORKING_DIRECTORY "${work}/devices" COMMAND_ERROR_IS_FATAL ANY)
    check(archive-devices "  _aux\\.txt\n" ${engine} -l "${work}/devices.zip")
endif()

# The checksum option, on the zip archive made above; CMake works the checksums out here. That a
# checksum matched is said only with -v, below the line that names the file; without -v the report
# starts as it always does. The last two are another file's checksum, after which nothing of the
# archive may be read, and one that cannot be a checksum.
set(zip "${work}/texts.zip")
file(MD5 "${zip}" md5)
file(SHA1 "${zip}" sha1)
file(SHA256 "${zip}" sha256)
file(SHA512 "${zip}" sha512)
file(SHA256 "${work}/texts.7z" other)
string(TOUPPER "${sha256}" upper)
check(checksum-md5    "MD5 of the file matches.*Tested: no errors"     ${engine} -v --checksum ${md5} -t "${zip}")
check(checksum-sha1   "SHA-1 of the file matches.*Tested: no errors"   ${engine} -v --checksum ${sha1} -t "${zip}")
check(checksum-sha256 "^Reading [^\n]*texts\\.zip\nSHA-256 of the file matches the checksum given\nArchive\n"
      ${engine} -v --checksum sha256:${sha256} -t "${zip}")
check(checksum-sha512 "SHA-512 of the file matches.*Tested: no errors" ${engine} -v --checksum ${sha512} -t "${zip}")
check(checksum-upper  "SHA-256 of the file matches.*Tested: no errors"
      ${engine} -v --checksum SHA-256:${upper} -t "${zip}")
check(checksum-silent "^Archive\n2 files, [^\n]*\nTested: no errors" ${engine} --checksum ${sha256} -t "${zip}")
check(checksum-other  "the SHA-256 of the file is ${sha256}, not ${other}"
      ${engine} --checksum ${other} -t "${zip}")
set_tests_properties(checksum-other PROPERTIES FAIL_REGULAR_EXPRESSION "Archive")
check(checksum-odd    "error: a checksum is" --checksum sha256:abc -t "${zip}")

# The output folder. One that holds something is refused unless --force is given, and the answer
# comes before the file is read. A folder that is new or empty is fine, -l and -t write nothing and
# so take any folder, and a file is no folder even with --force. The first two folders are removed
# before each run, and the empty one is made again.
add_test(NAME folder-clear
         COMMAND ${CMAKE_COMMAND} -E rm -rf "${work}/folder-new" "${work}/folder-empty" "${work}/burn-clash")
set_tests_properties(folder-clear PROPERTIES FIXTURES_SETUP folder-cleared)
add_test(NAME folder-make COMMAND ${CMAKE_COMMAND} -E make_directory "${work}/folder-empty")
set_tests_properties(folder-make PROPERTIES FIXTURES_REQUIRED folder-cleared FIXTURES_SETUP folders)
check(folder-new    "Extracted to " ${engine} -o "${work}/folder-new" "${zip}")
check(folder-empty  "Extracted to " ${engine} -o "${work}/folder-empty" "${zip}")
set_tests_properties(folder-new folder-empty PROPERTIES FIXTURES_REQUIRED folders)
check(folder-filled "^error: the output folder is not empty: [^\n]*use --force[^\n]*\nRun with --help"
      ${engine} -v -o "${work}" "${zip}")
check(folder-filled-test "Tested: no errors" ${engine} -t -o "${work}" "${zip}")
check(folder-file   "error: not a folder: " ${engine} --force -o "${zip}" "${zip}")
# The folder of a nested installer, with -r, is treated the same. It lies in a folder that was new,
# so it takes a bundle that itself has a payload in that folder: the installer is then reported as
# one that failed, by the folder and before it is read.
add_test(NAME burn-clash-make
         COMMAND makeburn "${work}/burn-clash.exe" sha1 broken.msi "broken.msi.extracted\\x.txt")
set_tests_properties(burn-clash-make PROPERTIES FIXTURES_SETUP burn-clash)
check(burn-clash "error: broken\\.msi: the output folder is not empty: [^\n]*broken\\.msi\\.extracted .use --force"
      ${engine} -r -o "${work}/burn-clash" "${work}/burn-clash.exe")
set_tests_properties(burn-clash PROPERTIES FIXTURES_REQUIRED "burn-clash;folders")

# How much is printed. By default the report, without the technical details that -v adds, such as
# the packages a bundle would run. With -q only the list of -l, and otherwise nothing at all.
# With -v the whole report is pinned for three formats: every line starts at the left margin, and
# only the items of a list are indented, below a line that ends in a colon. What -v adds comes
# right after the first line, so that a report ends alike with and without it.
set_tests_properties(burn-test PROPERTIES FAIL_REGULAR_EXPRESSION "Install chain:|engine|Bootstrapper|bytes|Reading ")
set(exact "16 payloads, 30\\.6 MiB .32133190 bytes.; 1 not embedded\nNot embedded:\n  redist[^\n]*\n")
set(chain "Install chain:\n  NetFx462Redist .ExePackage, 1 payload, not embedded.\n")
set(chain "${chain}  core\\.msi .MsiPackage, 2 payloads.\n")
set(parts "Burn engine 3\\.x\nBootstrapper application with 34 internal files\n1 attached container\n")
check(report-verbose "WiX Burn bundle\n${parts}${chain}.*\n${exact}Tested: all sizes"
      ${engine} -v -t "${burn}")
set(made "Setup data 7\\.1\\.1, compression lzma2, encryption none\n137 entries in .Files., 136 stored files\n")
set(held "136 files, 50\\.3 MiB .52782767 bytes.\nNot embedded:\n  Uninstaller .generated at install time.\n")
check(report-verbose-inno "^Reading [^\n]*\nInno Setup 7\\.1\\.1 - Inno Setup installer\n${made}${held}Tested: all"
      ${no_engine} -v -t "${inno}")
set(held "Code page 1252\n1 cabinet\n1 file, 40\\.0 MiB .41905976 bytes.\n")
check(report-verbose-msi "^Reading [^\n]*\nGitHub CLI 2\\.105\\.0 - Windows Installer package\n${held}Tested: no errors"
      ${engine} -v -t "${msi}")
check(report-quiet-list "^ +[0-9]+  LICENSE\\.txt" ${engine} -q -l "${zip}")
set_tests_properties(report-quiet-list PROPERTIES FAIL_REGULAR_EXPRESSION "Archive|files, ")
add_test(NAME report-quiet-test COMMAND instext ${engine} -q -t "${zip}")
set_tests_properties(report-quiet-test PROPERTIES FAIL_REGULAR_EXPRESSION ".")
# The output folder is shown the way the command line names it, whatever the current folder is: by
# the name given with -o, also for what is written inside it, and without -o by the installer's
# name as it was given. These two run in a folder of their own and name everything through "..".
file(MAKE_DIRECTORY "${work}/elsewhere")
set(named "\\.\\..out-named")
check(report-folder-named "Extracted to ${named}; [^\n]*\nWrote 1 internal file to ${named}._internals\n"
      ${engine} --force -i -o ../out-named ../burn-names.exe)
check(report-folder-default "Extracted to \\.\\..texts\\.zip\\.extracted\n" ${engine} --force ../texts.zip)
set_tests_properties(report-folder-named report-folder-default PROPERTIES WORKING_DIRECTORY "${work}/elsewhere")
set_tests_properties(report-folder-named PROPERTIES FIXTURES_REQUIRED burn-names)

# The command line. The name in three scripts is there for Windows, where arguments and messages
# pass through conversions that plain ASCII text never shows to be wrong.
set(unicode "${work}/Blåbærsyltetøy Ωμέγα 日本語")
file(COPY_FILE "${work}/texts.7z" "${unicode}.7z")
set(usage "Usage:\n  instext .OPTIONS. FILE\n\nOptions:\n")
check(cli-help       "^InstExt ${PROJECT_VERSION}\n\nExtracts .*\n\n${usage}.*--7zip FILE" --help)
check(cli-version    "^InstExt ${PROJECT_VERSION}\n" --version)
check(cli-no-argument "error: no file given.*Run with --help")
check(cli-two-files  "error: only one file can be given.*Run with --help" one.exe two.exe)
check(cli-no-file    "error: no such file: .*Blåbærsyltetøy Ωμέγα 日本語\\.exe" "${unicode}.exe")
check(cli-bad-option "error: .*--bogus.*Run with --help" --bogus "${work}/texts.7z")
check(cli-shortened  "error: .*--lis" --lis "${work}/texts.7z")
check(cli-unicode    "Extracted to .*Blåbærsyltetøy Ωμέγα 日本語"
      ${engine} --force --output-dir "${unicode}" "${unicode}.7z")
check(cli-both       "error: .*exclude each other" -q -v "${work}/texts.7z")

# The version. Alone, --version prints one line and does not look for the 7-Zip engine. With -v it
# adds what the program was built with and the engine it finds, with the version that the engine
# reports about itself (those before 7-Zip 23.01 do not tell). Having no engine is no failure there:
# the run says where it looked and ends well, which the last test checks by its exit status alone.
set_tests_properties(cli-version PROPERTIES FAIL_REGULAR_EXPRESSION "Built with|Linked|7-Zip")
set(built "Built with [^\n]+\nLinked libraries:\n  bit7z [0-9.]+\n  Boost [0-9.]+\n  bzip2 [0-9.]+\n")
set(built "${built}  liblzma [^\n]+\n  pugixml [0-9.]+\n  zlib [^\n]+\n")
set(loaded "External 7-Zip engine ([0-9]+\\.[0-9]+|of unknown version) loaded from:\n  [^\n]+\n")
check(cli-version-verbose "^InstExt ${PROJECT_VERSION}\n${built}${loaded}" ${engine} --version --verbose)
check(cli-version-no-engine "\nExternal 7-Zip engine could not be loaded from:\n  [^\n]*no-such-engine\n"
      ${no_engine} --version -v)
add_test(NAME cli-version-status COMMAND instext ${no_engine} --version -v)
