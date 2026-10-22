add_executable(OtaBootSwitchModelTest
  OtaBootSwitchModelTest.cpp
  ${REPO_ROOT}/src/network/OtaBootSwitchModel.cpp
)

target_include_directories(OtaBootSwitchModelTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(OtaBootSwitchModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OtaBootSwitchModelTest)
