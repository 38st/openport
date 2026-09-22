add_executable(openport-probe probe/main.cpp)
target_link_libraries(openport-probe PRIVATE openport::providers openport::options)
