add_executable(openport_tests
  pricing/black_test.cpp
  pricing/implied_vol_test.cpp
  pricing/binomial_test.cpp
  pricing/inverse_test.cpp
)
target_link_libraries(openport_tests PRIVATE openport::core openport::options GTest::gtest_main)
target_compile_definitions(openport_tests PRIVATE
  OPENPORT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

include(GoogleTest)
gtest_discover_tests(openport_tests DISCOVERY_TIMEOUT 60)
