#
# project: lua
#
cmake_minimum_required(VERSION 2.8)
# include the fips main cmake file
get_filename_component(FIPS_ROOT_DIR "../fips" ABSOLUTE)
include("${FIPS_ROOT_DIR}/cmake/fips.cmake")

include_directories(src)

fips_setup()
fips_project(lua)
fips_add_subdirectory(lua-5.1.4)
fips_add_subdirectory(lua-5.3.5)
fips_add_subdirectory(LuaJIT)
fips_finish()


