add_executable(openport_tests
  pricing/black_test.cpp
  pricing/implied_vol_test.cpp
  pricing/binomial_test.cpp
  md/time_test.cpp
  md/contract_test.cpp
  md/event_queue_test.cpp
  analytics/forward_test.cpp
  analytics/chain_analytics_test.cpp
  analytics/svi_test.cpp
  analytics/ssvi_test.cpp
  analytics/american_test.cpp
)
target_link_libraries(openport_tests PRIVATE openport::options GTest::gtest_main)
target_include_directories(openport_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(openport_tests PRIVATE
  OPENPORT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

if(OPENPORT_BUILD_PROVIDERS)
  target_sources(openport_tests PRIVATE
    md/recording_test.cpp
    providers/replay_test.cpp
    server/recording_test.cpp
    net/http_test.cpp
    providers/cboe_test.cpp
    providers/runtime_test.cpp
    providers/cli_test.cpp
    providers/massive_test.cpp
    providers/thetadata_test.cpp
    server/api_test.cpp
    server/candles_test.cpp
    server/web_policy_test.cpp
    server/engine_test.cpp
    server/paper_test.cpp
  )
  target_compile_definitions(openport_tests PRIVATE OPENPORT_APPS_DIR="${PROJECT_BINARY_DIR}/apps")
  add_dependencies(openport_tests openportd openport-probe)
  target_link_libraries(openport_tests PRIVATE openport::providers openport::server
                        nlohmann_json::nlohmann_json)
  if(OPENPORT_WITH_DATABENTO)
    target_sources(openport_tests PRIVATE providers/databento_test.cpp)
  endif()
else()
  target_link_libraries(openport_tests PRIVATE openport::core)
endif()

# Paper trading core and the reusable scripted market fixture.
target_sources(openport_tests PRIVATE
  trading/money_ledger_test.cpp
  trading/matching_test.cpp
  trading/risk_settlement_test.cpp
  trading/journal_test.cpp
  trading/evaluation_test.cpp
  trading/conditional_test.cpp
  trading/funded_test.cpp
  trading/multileg_test.cpp
  trading/orders_test.cpp
)
target_link_libraries(openport_tests PRIVATE openport::trading nlohmann_json::nlohmann_json)

include(GoogleTest)
gtest_discover_tests(openport_tests DISCOVERY_TIMEOUT 60)
