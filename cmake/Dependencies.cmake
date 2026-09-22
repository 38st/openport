# Third-party dependencies.
#
# Small header-mostly libraries are fetched at configure time, pinned by version
# and checksum so every build sees the same code. Boost and OpenSSL are large,
# so they come from the system package manager (Homebrew or apt).

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Homebrew installs OpenSSL keg-only; point CMake at it on macOS.
if(APPLE AND NOT OPENSSL_ROOT_DIR)
  foreach(prefix /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
    if(EXISTS "${prefix}/include/openssl/ssl.h")
      set(OPENSSL_ROOT_DIR "${prefix}")
      break()
    endif()
  endforeach()
endif()
find_package(OpenSSL 3 REQUIRED)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_Declare(unordered_dense
  URL https://github.com/martinus/unordered_dense/archive/refs/tags/v5.0.1.tar.gz
  URL_HASH SHA256=b79f46db45fd73310211429e5d33da4a579543ac544ae7dc12f8d9a31ad0aea4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_Declare(simdjson
  URL https://github.com/simdjson/simdjson/archive/refs/tags/v4.6.11.tar.gz
  URL_HASH SHA256=61d948fc24f0d793829ad658058e7597d064988a89b4607ea02e401a82df98ff
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(SIMDJSON_DEVELOPER_MODE OFF CACHE INTERNAL "")
set(SIMDJSON_BUILD_STATIC_LIB ON CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json unordered_dense simdjson)

if(OPENPORT_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE INTERNAL "")
  set(gtest_force_shared_crt ON CACHE INTERNAL "")
  FetchContent_MakeAvailable(googletest)
endif()

if(OPENPORT_BUILD_BENCHMARKS)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
    URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE INTERNAL "")
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE INTERNAL "")
  set(BENCHMARK_ENABLE_WERROR OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(benchmark)
endif()

if(OPENPORT_BUILD_DAEMON)
  find_package(Boost 1.83 CONFIG REQUIRED)
endif()

if(OPENPORT_BUILD_PYTHON)
  find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)
  FetchContent_Declare(pybind11
    URL https://github.com/pybind11/pybind11/archive/refs/tags/v3.1.0.tar.gz
    URL_HASH SHA256=d5558cd419c8d46bdc958064cb97f963d1ea793866414c025906ec15033512ed
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(pybind11)
endif()
