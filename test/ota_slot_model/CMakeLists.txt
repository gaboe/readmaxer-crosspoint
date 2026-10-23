add_executable(OtaSlotModelTest
  OtaSlotModelTest.cpp
  ${REPO_ROOT}/src/network/OtaSlotModel.cpp
)

target_include_directories(OtaSlotModelTest PRIVATE
  ${REPO_ROOT}/src/network
)

target_link_libraries(OtaSlotModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OtaSlotModelTest)
