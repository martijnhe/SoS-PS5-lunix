# Linux launcher: a GTK 3 settings window like the Windows launcher. GTK cannot be linked
# statically, so the top-level "-static" is removed here; libstdc++ and libgcc stay static.
string(REPLACE "-static " "" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ")
string(STRIP "${CMAKE_EXE_LINKER_FLAGS}" CMAKE_EXE_LINKER_FLAGS)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
find_package(Threads REQUIRED)

add_executable(SonsOfSparta-PS5 launcher_linux.cpp MissingImportStubs.cpp)
target_link_libraries(SonsOfSparta-PS5 PRIVATE PkgConfig::GTK3 Threads::Threads ${CMAKE_DL_LIBS})
set_target_properties(SonsOfSparta-PS5 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
