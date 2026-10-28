// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "sql/parser/ob_parser.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
TEST(FileSqlParser, interfaces_and_ordinary_sql)
{
  const char *queries[] = {
    "SELECT * FROM read_csv('/tmp/a.csv')",
    "SELECT a.id FROM read_csv('/tmp/a.csv', header=true, delimiter=';', sample_size=-1) AS a",
    "SELECT * FROM read_json('/tmp/a.jsonl') j",
    "SELECT * FROM read_ndjson('/tmp/a.jsonl')",
    "SELECT * FROM read_parquet('/tmp/a.parquet')",
    "SELECT file FROM glob('/tmp/*.csv')",
    "DESCRIBE SELECT * FROM read_csv('/tmp/a.csv')",
    "EXPLAIN SELECT a.id FROM read_csv('/tmp/a.csv') a LEFT JOIN read_json('/tmp/b.jsonl') b ON a.id=b.id",
    "COPY t TO '/tmp/t.csv' (FORMAT CSV, HEADER, DELIMITER ';', QUOTE '\"', NULL 'NA')",
    "COPY (SELECT id, SUM(x) AS total FROM read_csv('/tmp/a.csv') GROUP BY id) TO '/tmp/result.csv' (header=true)",
    "SELECT 1", "SELECT * FROM t WHERE id=1 ORDER BY id LIMIT 2",
    "DESCRIBE SELECT 1", "DESCRIBE t", "EXPLAIN SELECT * FROM t",
    "SELECT read_csv FROM t", "SELECT copy FROM t"
  };
  for (const char *query : queries) {
    ObArenaAllocator allocator;
    ObParser parser(allocator, 0);
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString::make_string(query), result)) << query;
    ASSERT_NE(nullptr, result.result_tree_);
    parser.free_result(result);
  }
}
int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
