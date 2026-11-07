cmake_minimum_required(VERSION 3.16)
project(joowy_memcached LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# Optional sanitizers, e.g. -DSANITIZE=thread or -DSANITIZE=address,undefined
set(SANITIZE "" CACHE STRING "Comma-separated list passed to -fsanitize")
if(SANITIZE)
  add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${SANITIZE})
endif()

add_library(cache cache.cpp)
target_include_directories(cache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cache PUBLIC Threads::Threads)

add_library(network network.cpp)
target_include_directories(network PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(memcached_server server.cpp protocol.cpp)
target_link_libraries(memcached_server PRIVATE cache network Threads::Threads)

add_executable(memcached_cli cli.cpp)
target_link_libraries(memcached_cli PRIVATE network)

# Stress tools; run them directly for longer runs (see README).
add_executable(cache_stress tests/cache_stress.cpp)
target_link_libraries(cache_stress PRIVATE cache Threads::Threads)

add_executable(net_stress tests/net_stress.cpp)
target_link_libraries(net_stress PRIVATE Threads::Threads)

include(CTest)
if(BUILD_TESTING)
  add_executable(cache_test tests/cache_test.cpp)
  target_link_libraries(cache_test PRIVATE cache Threads::Threads)
  add_test(NAME cache_test COMMAND cache_test)

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME protocol_test
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol_test.py
            $<TARGET_FILE:memcached_server> $<TARGET_FILE:memcached_cli>)
  set_tests_properties(protocol_test PROPERTIES TIMEOUT 30)

  add_test(NAME cache_stress_smoke COMMAND cache_stress 0.2)
  set_tests_properties(cache_stress_smoke PROPERTIES TIMEOUT 60)
  add_test(NAME net_stress_smoke
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/net_stress_smoke.py
            $<TARGET_FILE:memcached_server> $<TARGET_FILE:net_stress>)
  set_tests_properties(net_stress_smoke PROPERTIES TIMEOUT 60)
endif()
