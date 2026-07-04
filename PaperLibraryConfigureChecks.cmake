
# Check whether malloc_trim(3) is supported.
include(CheckSymbolExists)
check_symbol_exists(malloc_trim "malloc.h" HAVE_MALLOC_TRIM)

# at the end, output the configuration
configure_file(
   ${CMAKE_CURRENT_SOURCE_DIR}/config-paperlibrary.h.cmake
   ${CMAKE_CURRENT_BINARY_DIR}/config-paperlibrary.h
)
