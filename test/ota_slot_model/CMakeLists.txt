add_executable(OtaSlotModelTest
  OtaSlotModelTest.cpp
  OtaWriteInvariantTest.cpp
  ${REPO_ROOT}/src/network/OtaSlotModel.cpp
  ${REPO_ROOT}/src/network/GuestSlotModel.cpp
)

target_include_directories(OtaSlotModelTest PRIVATE
  ${REPO_ROOT}/src/network
)

target_link_libraries(OtaSlotModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OtaSlotModelTest)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME SleepRefusesUnsafeBootSelection
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/SleepGateTest.py
          ${REPO_ROOT}/src/main.cpp ${CMAKE_CXX_COMPILER})

add_test(NAME OtaBootSwitchFlashFailures
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/OtaBootSwitchTest.py
          ${REPO_ROOT} ${CMAKE_CXX_COMPILER})
