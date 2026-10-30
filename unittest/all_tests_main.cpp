// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
// Shared entry point for GoogleTest and the format reader's fixture CLI.
#ifdef FILE_SQL_READER_TEST_ENTRY
int run_file_scan_candidate(int argc, char **argv);
int main(int argc, char **argv)
{
  return run_file_scan_candidate(argc, argv);
}
#else
#include <gtest/gtest.h>
int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
