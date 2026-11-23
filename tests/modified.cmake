# Test helper: fails unless a file was last modified in a given year. "now" is the current year.
#   cmake -Dfile=<file> -Dyear=<year> -P modified.cmake
if(year STREQUAL "now")
    string(TIMESTAMP year "%Y" UTC)
endif()
file(TIMESTAMP "${file}" modified "%Y" UTC)
if(NOT modified STREQUAL year)
    message(FATAL_ERROR "${file} was last modified in the year '${modified}', not in ${year}")
endif()
