add_executable(RetainedCaptureTest RetainedCaptureTest.cpp ../../src/network/RetainedCapture.cpp)
target_link_libraries(RetainedCaptureTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_definitions(RetainedCaptureTest PRIVATE RTC_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/telemetry-rtc-v2.hex")
gtest_discover_tests(RetainedCaptureTest)
