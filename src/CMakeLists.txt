# Pricing, analytics and the normalised market-data model: no network dependencies.
add_library(openport_core
  pricing/black.cpp
  pricing/implied_vol.cpp
  pricing/binomial.cpp
  md/time.cpp
  md/contract.cpp
  analytics/chain_book.cpp
  analytics/forward.cpp
  analytics/chain_analytics.cpp
  analytics/svi.cpp
  analytics/ssvi.cpp
)
add_library(openport::core ALIAS openport_core)
target_include_directories(openport_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(openport_core PRIVATE openport::options)
set_target_properties(openport_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Pure paper-trading reducer; only the journal performs file I/O.
if(NOT TARGET OpenSSL::Crypto)
  find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
endif()
add_library(openport_trading
  trading/money.cpp
  trading/types.cpp
  trading/dividends.cpp
  trading/ledger.cpp
  trading/risk.cpp
  trading/journal.cpp
  trading/session.cpp
  trading/state_delta.cpp
  trading/history.cpp
  trading/margin.cpp
)
add_library(openport::trading ALIAS openport_trading)
target_include_directories(openport_trading PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(openport_trading PUBLIC openport::core
  PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json openport::options)
set_target_properties(openport_trading PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(OPENPORT_BUILD_PROVIDERS)
  # Recording is independent of Databento, including builds without its adapter.
  find_path(OPENPORT_ZSTD_INCLUDE_DIR zstd.h REQUIRED)
  if(OPENPORT_STATIC_DEPS)
    find_library(OPENPORT_ZSTD_LIBRARY NAMES libzstd.a REQUIRED)
  else()
    find_library(OPENPORT_ZSTD_LIBRARY NAMES zstd REQUIRED)
  endif()
  add_library(openport_recording md/recording.cpp)
  add_library(openport::recording ALIAS openport_recording)
  target_include_directories(openport_recording PRIVATE ${OPENPORT_ZSTD_INCLUDE_DIR})
  target_link_libraries(openport_recording PUBLIC openport::core PRIVATE ${OPENPORT_ZSTD_LIBRARY} Threads::Threads openport::options)

  # HTTPS and WebSocket plumbing shared by the provider adapters.
  add_library(openport_net
    net/http.cpp
  )
  add_library(openport::net ALIAS openport_net)
  target_compile_definitions(openport_net PRIVATE OPENPORT_VERSION="${PROJECT_VERSION}")
  target_include_directories(openport_net PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
  target_link_libraries(openport_net
    PUBLIC Boost::headers OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads
    PRIVATE openport::options)

  # One adapter per market-data provider.
  add_library(openport_providers
    providers/factory.cpp
    providers/demo.cpp
    providers/replay.cpp
    providers/snapshot.cpp
    providers/cboe.cpp
    providers/massive.cpp
    providers/thetadata.cpp
  )
  add_library(openport::providers ALIAS openport_providers)
  target_link_libraries(openport_providers PUBLIC openport::recording)
  target_link_libraries(openport_providers
    PUBLIC openport::core openport::net
    PRIVATE simdjson::simdjson openport::options)

  if(OPENPORT_WITH_DATABENTO)
    target_sources(openport_providers PRIVATE providers/databento.cpp)
    target_link_libraries(openport_providers PUBLIC databento::databento)
    target_compile_definitions(openport_providers PUBLIC OPENPORT_WITH_DATABENTO)
  endif()
endif()

if(OPENPORT_BUILD_PROVIDERS)
  # The engine thread, JSON API and HTTP/WebSocket server behind openportd.
  add_library(openport_server
    server/candles.cpp
    server/engine.cpp
    server/paper_engine.cpp
    server/paper_api.cpp
    server/plans.cpp
    server/api.cpp
    server/replay_host.cpp
    server/web_server.cpp
  )
  add_library(openport::server ALIAS openport_server)
  # Replays run the replay provider beside the live feed.
  target_link_libraries(openport_server PUBLIC openport::recording openport::trading openport::providers)
  target_link_libraries(openport_server
    PUBLIC openport::core openport::net
    PRIVATE nlohmann_json::nlohmann_json openport::options)
endif()
