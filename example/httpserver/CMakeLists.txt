add_executable(httpserver httpserver.cpp)
add_dependencies(httpserver net4cxx)
target_link_libraries(httpserver net4cxx)