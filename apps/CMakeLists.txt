add_library(mf_app_common STATIC
  common/local_api.cpp
  common/remote_api.cpp)
target_include_directories(mf_app_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mf_app_common PUBLIC mf)
target_link_libraries(mf_app_common PRIVATE mf_warnings)
set_target_properties(mf_app_common PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_executable(mfctl mfctl/main.cpp)
target_link_libraries(mfctl PRIVATE mf_app_common mf_warnings)

add_executable(mfd mfd/main.cpp)
target_link_libraries(mfd PRIVATE mf_app_common mf_warnings)

add_executable(mf-drain-sim drain_sim/main.cpp)
target_link_libraries(mf-drain-sim PRIVATE mf_app_common mf_warnings)
