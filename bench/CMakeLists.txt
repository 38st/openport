add_executable(openport_bench
  bench_pricing.cpp
)
target_link_libraries(openport_bench PRIVATE openport::core openport::options benchmark::benchmark_main)

add_executable(openport_measure_svi measure_svi.cpp)
target_link_libraries(openport_measure_svi PRIVATE openport::options)
if(OPENPORT_BUILD_PROVIDERS)
  target_link_libraries(openport_measure_svi PRIVATE openport::server)
  target_compile_definitions(openport_measure_svi PRIVATE OPENPORT_SVI_API_BENCH)
else()
  target_link_libraries(openport_measure_svi PRIVATE openport::core)
endif()

if(OPENPORT_BUILD_PROVIDERS)
  add_executable(openport_bench_trading bench_trading.cpp)
  target_link_libraries(openport_bench_trading PRIVATE openport::server openport::options benchmark::benchmark_main)
endif()
