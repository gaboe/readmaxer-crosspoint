add_executable(GuestSlotTest
  GuestSlotTest.cpp
  ${REPO_ROOT}/src/network/GuestSlotModel.cpp
  ${REPO_ROOT}/src/network/FirmwareHostTag.cpp
)

target_include_directories(GuestSlotTest PRIVATE ${REPO_ROOT}/src/network)
target_link_libraries(GuestSlotTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(GuestSlotTest)
