# Local Linux mysqltest deployment. Configure secure_file_priv to this directory
# before starting the test connection; this include never widens that policy.
--let $file_sql_dir = /tmp/seekdb_file_sql_mysqltest
--exec mkdir -p "$file_sql_dir"
--copy_file mysql_test/test_suite/file_sql/data/users.csv $file_sql_dir/users.csv
--copy_file mysql_test/test_suite/file_sql/data/teams.jsonl $file_sql_dir/teams.jsonl
--copy_file mysql_test/test_suite/file_sql/data/numbers.parquet $file_sql_dir/numbers.parquet
--copy_file mysql_test/test_suite/file_sql/data/bad.csv $file_sql_dir/bad.csv
--copy_file mysql_test/test_suite/file_sql/data/nested.jsonl $file_sql_dir/nested.jsonl
