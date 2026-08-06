cmake_minimum_required(VERSION 3.24)

project(TerminalChat LANGUAGES CXX)

set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v5.0.0
)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(ftxui nlohmann_json)

file(GLOB_RECURSE CHAT_SHARED_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/*.cxx"
)

file(GLOB_RECURSE CHAT_SERVER_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/server/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/server/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/server/*.cxx"
)

file(GLOB_RECURSE CHAT_CLIENT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/client/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/client/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/client/*.cxx"
)

add_executable(chat_server)
target_sources(chat_server PRIVATE
  ${CHAT_SERVER_SOURCES}
  ${CHAT_SHARED_SOURCES}
)

add_executable(chat_client)
target_sources(chat_client PRIVATE
  ${CHAT_CLIENT_SOURCES}
  ${CHAT_SHARED_SOURCES}
)

set_target_properties(chat_server PROPERTIES LINKER_LANGUAGE CXX)
set_target_properties(chat_client PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(chat_server PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/server"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/shared"
)

target_include_directories(chat_client PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/client"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/shared"
)

target_link_libraries(chat_server PRIVATE
  SQLite::SQLite3
  PkgConfig::SODIUM
  Threads::Threads
  ftxui::screen
  ftxui::dom
  ftxui::component
  nlohmann_json::nlohmann_json
)

target_link_libraries(chat_client PRIVATE
  SQLite::SQLite3
  PkgConfig::SODIUM
  Threads::Threads
  ftxui::screen
  ftxui::dom
  ftxui::component
  nlohmann_json::nlohmann_json
)