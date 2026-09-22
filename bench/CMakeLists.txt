add_executable(openport_bench
  bench_pricing.cpp
)
target_link_libraries(openport_bench PRIVATE openport::core openport::options benchmark::benchmark_main)
