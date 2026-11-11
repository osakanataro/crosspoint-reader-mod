add_executable(PrintfConversionsTest
  PrintfConversionsTest.cpp
)

target_include_directories(PrintfConversionsTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(PrintfConversionsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PrintfConversionsTest)
