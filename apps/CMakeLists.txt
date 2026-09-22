add_executable(openport-probe probe/main.cpp)
target_link_libraries(openport-probe PRIVATE openport::providers openport::options)

add_executable(openportd openportd/main.cpp)
target_link_libraries(openportd PRIVATE openport::server openport::providers openport::options)
