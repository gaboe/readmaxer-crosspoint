find_package(OpenSSL REQUIRED)
find_path(ARDUINOJSON_INCLUDE_DIR ArduinoJson.h
  PATHS ${REPO_ROOT}/.pio/libdeps/x4pro/ArduinoJson/src)
if(NOT ARDUINOJSON_INCLUDE_DIR)
  FetchContent_Declare(ArduinoJson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2)
  FetchContent_MakeAvailable(ArduinoJson)
  set(ARDUINOJSON_INCLUDE_DIR ${arduinojson_SOURCE_DIR}/src)
endif()
add_executable(GuestInstallTest
  GuestInstallTest.cpp
  ${REPO_ROOT}/src/network/GuestInstall.cpp
  ${REPO_ROOT}/src/network/GuestInstallModel.cpp
  ${REPO_ROOT}/src/network/GuestSlotModel.cpp
  ${REPO_ROOT}/src/network/FirmwareHostTag.cpp
  ${REPO_ROOT}/src/network/OtaSlotModel.cpp
)
target_include_directories(GuestInstallTest PRIVATE
  stubs ${REPO_ROOT}/src/network ${REPO_ROOT}/lib/Memory ${ARDUINOJSON_INCLUDE_DIR})
target_compile_definitions(GuestInstallTest PRIVATE FREEINK_DEVICE_X4PRO=1)
target_link_libraries(GuestInstallTest PRIVATE crosspoint_test_common GTest::gtest_main OpenSSL::Crypto)
gtest_discover_tests(GuestInstallTest)
