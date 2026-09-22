# Validation suite. Every test binary runs plainly: no watchdogs, no timeouts.
function(mf_add_test name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_link_libraries(${name} PRIVATE mf_app_common mf_warnings)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 0)
endfunction()

mf_add_test(test_core test_core.cpp)
mf_add_test(test_domain test_domain.cpp)
mf_add_test(test_engine test_engine.cpp)
mf_add_test(test_store test_store.cpp)
mf_add_test(test_controller test_controller.cpp)
mf_add_test(test_property test_property.cpp)
mf_add_test(test_adversarial test_adversarial.cpp)
mf_add_test(test_concurrency test_concurrency.cpp)

if(WIN32 OR UNIX)
  mf_add_test(test_transport test_transport.cpp)
  target_compile_definitions(test_transport PRIVATE
    MF_TEST_BINDIR="${CMAKE_BINARY_DIR}/apps"
    MF_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
