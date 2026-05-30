cmake_minimum_required(VERSION 3.16)
project(git_gencommit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)

add_executable(git-gencommit
  src/main.cpp
  src/cli.cpp
  src/config.cpp
  src/config_wizard.cpp
  src/toml_lite.cpp
  src/json.cpp
  src/hf.cpp
  src/git.cpp
  src/llm.cpp
)

target_include_directories(git-gencommit PRIVATE src)
target_link_libraries(git-gencommit PRIVATE CURL::libcurl)

if(MSVC)
  target_compile_options(git-gencommit PRIVATE /W4)
else()
  target_compile_options(git-gencommit PRIVATE -Wall -Wextra -Wpedantic)
endif()
