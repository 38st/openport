# Warning, sanitizer and optimisation flags shared by every OpenPort target.

add_library(openport_options INTERFACE)
add_library(openport::options ALIAS openport_options)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(openport_options INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Woverloaded-virtual -Wdouble-promotion
    # GCC reports false positives inside Boost.Asio templates with this one.
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wnull-dereference>
    -Wformat=2 -Wimplicit-fallthrough -Wmisleading-indentation)
  if(OPENPORT_WERROR)
    target_compile_options(openport_options INTERFACE -Werror)
  endif()

  if(OPENPORT_SANITIZE)
    target_compile_options(openport_options INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    target_link_options(openport_options INTERFACE -fsanitize=address,undefined)
  endif()

  if(OPENPORT_NATIVE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
      target_compile_options(openport_options INTERFACE -mcpu=native)
    else()
      target_compile_options(openport_options INTERFACE -march=native)
    endif()
  endif()
elseif(MSVC)
  target_compile_options(openport_options INTERFACE /W4 /permissive-)
endif()
