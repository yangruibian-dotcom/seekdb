// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#pragma once
#include "sql/engine/basic/file_sql.h"

namespace oceanbase { namespace sql {
class ObSQLSessionInfo;
int file_sql_access(ObSQLSessionInfo *session, std::string &root);
int file_sql_error(const std::exception &error);
struct FileSqlState {
  file_sql::Binding binding;
  std::unique_ptr<file_sql::Scan> scan;
  file_sql::Row row;
  std::vector<std::string> files;
  size_t index = 0;
};
struct FileCopyState {
  file_sql::Binding binding;
  std::unique_ptr<file_sql::CsvOutput> output;
  std::vector<char> buffer;
  int64_t count = 0;
  bool finished = false;
};
}}
