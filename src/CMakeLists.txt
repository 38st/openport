# Pricing, analytics and the normalised market-data model: no network dependencies.
add_library(openport_core
  pricing/black.cpp
  pricing/implied_vol.cpp
  pricing/binomial.cpp
  md/time.cpp
  md/contract.cpp
)
add_library(openport::core ALIAS openport_core)
target_include_directories(openport_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(openport_core PRIVATE openport::options)
set_target_properties(openport_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(OPENPORT_BUILD_PROVIDERS)
  # HTTPS and WebSocket plumbing shared by the provider adapters.
  add_library(openport_net
    net/http.cpp
  )
  add_library(openport::net ALIAS openport_net)
  target_include_directories(openport_net PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
  target_link_libraries(openport_net
    PUBLIC Boost::headers OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads
    PRIVATE openport::options)

  # One adapter per market-data provider.
  add_library(openport_providers
    providers/factory.cpp
    providers/cboe.cpp
  )
  add_library(openport::providers ALIAS openport_providers)
  target_link_libraries(openport_providers
    PUBLIC openport::core openport::net
    PRIVATE simdjson::simdjson openport::options)

  if(OPENPORT_WITH_DATABENTO)
    target_sources(openport_providers PRIVATE providers/databento.cpp)
    target_link_libraries(openport_providers PUBLIC databento::databento)
    target_compile_definitions(openport_providers PUBLIC OPENPORT_WITH_DATABENTO)
  endif()
endif()
