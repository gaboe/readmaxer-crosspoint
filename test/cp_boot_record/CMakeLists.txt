add_executable(CpBootRecordTest CpBootRecordTest.cpp ../../lib/CpBootRecord/CpBootRecord.cpp)
target_link_libraries(CpBootRecordTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CpBootRecordTest)
