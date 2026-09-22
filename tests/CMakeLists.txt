add_executable(openport_tests
  pricing/black_test.cpp
  pricing/implied_vol_test.cpp
  pricing/binomial_test.cpp
  md/time_test.cpp
  md/contract_test.cpp
  analytics/forward_test.cpp
  analytics/chain_analytics_test.cpp
)
target_link_libraries(openport_tests PRIVATE openport::options GTest::gtest_main)
target_include_directories(openport_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(openport_tests PRIVATE
  OPENPORT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

if(OPENPORT_BUILD_PROVIDERS)
  target_sources(openport_tests PRIVATE
    net/http_test.cpp
    providers/cboe_test.cpp
    providers/massive_test.cpp
    providers/thetadata_test.cpp
    server/api_test.cpp
  )
  target_link_libraries(openport_tests PRIVATE openport::providers openport::server
                        nlohmann_json::nlohmann_json)
  if(OPENPORT_WITH_DATABENTO)
    target_sources(openport_tests PRIVATE providers/databento_test.cpp)
  endif()
else()
  target_link_libraries(openport_tests PRIVATE openport::core)
endif()

include(GoogleTest)
gtest_discover_tests(openport_tests DISCOVERY_TIMEOUT 60)
