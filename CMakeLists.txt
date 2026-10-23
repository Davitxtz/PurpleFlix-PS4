cmake_minimum_required(VERSION 3.10)
project(purpleflix)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp)
add_executable(purpleflix ${SOURCES})

# SDL2: tenta pkg-config; se não achar, tenta -lSDL2 direto.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PF_SDL2 QUIET sdl2)
endif()
if(PF_SDL2_FOUND)
  message(STATUS "SDL2 via pkg-config")
  target_include_directories(purpleflix PRIVATE ${PF_SDL2_INCLUDE_DIRS})
  if(PF_SDL2_STATIC_LDFLAGS)
    target_link_libraries(purpleflix ${PF_SDL2_STATIC_LDFLAGS})
  else()
    target_link_libraries(purpleflix ${PF_SDL2_LDFLAGS})
  endif()
else()
  message(STATUS "sdl2.pc nao encontrado; usando -lSDL2")
  target_link_libraries(purpleflix SDL2)
endif()

target_link_libraries(purpleflix ScePad SceUserService)

# Funções do toolchain PacBrew/OpenOrbis (ver ps4-openorbis-sample).
add_self(purpleflix)
add_pkg(purpleflix ${CMAKE_SOURCE_DIR}/data "PFLX00001" "PurpleFlix by Davitxtz" "01.03")
