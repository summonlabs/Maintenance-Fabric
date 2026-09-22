foreach(example basic_flow custom_policy restart_recovery)
  add_executable(mf_example_${example} ${example}.cpp)
  target_include_directories(mf_example_${example} PRIVATE ${CMAKE_SOURCE_DIR})
  target_link_libraries(mf_example_${example} PRIVATE mf mf_warnings)
endforeach()

if(MF_BUILD_TESTS)
  add_test(NAME mf_example_basic_flow COMMAND mf_example_basic_flow)
  add_test(NAME mf_example_custom_policy COMMAND mf_example_custom_policy)
  add_test(NAME mf_example_restart_recovery COMMAND mf_example_restart_recovery)
  set_tests_properties(mf_example_basic_flow mf_example_custom_policy
    mf_example_restart_recovery PROPERTIES TIMEOUT 0)
endif()
