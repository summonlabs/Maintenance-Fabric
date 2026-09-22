add_executable(mf_bench bench.cpp)
target_include_directories(mf_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mf_bench PRIVATE mf mf_warnings)

install(TARGETS mf_bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
