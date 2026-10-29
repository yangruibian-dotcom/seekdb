// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/file_sql_native.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_schema_utils.h"
#include "lib/allocator/page_arena.h"

namespace oceanbase { namespace sql {
using namespace common;
int file_sql_access(ObSQLSessionInfo *session, std::string &root)
{
  using namespace common;
  int ret = OB_SUCCESS;
  ObObj enabled;
  ObString secure;
  ObArenaAllocator allocator("FileSqlAccess");
  if (OB_ISNULL(session) || OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled.get_bool()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL is disabled; SET enable_file_sql = ON");
  } else if (OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
      *GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV, allocator, secure))) {
  } else {
    if (secure.empty()) root.clear();
    else {
      try { root.assign(secure.ptr(), secure.length()); }
      catch (const std::bad_alloc &) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    }
  }
  return ret;
}
int file_sql_error(const std::exception &error)
{
  LOG_USER_ERROR(OB_NOT_SUPPORTED, error.what());
  return common::OB_NOT_SUPPORTED;
}
}}
