add_library(openport_core
  pricing/black.cpp
  pricing/implied_vol.cpp
  pricing/binomial.cpp
  pricing/inverse.cpp
)
add_library(openport::core ALIAS openport_core)

target_include_directories(openport_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(openport_core PRIVATE openport::options)
set_target_properties(openport_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
