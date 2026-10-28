--remove_file $file_sql_dir/users.csv
--remove_file $file_sql_dir/teams.jsonl
--remove_file $file_sql_dir/numbers.parquet
--remove_file $file_sql_dir/bad.csv
--remove_file $file_sql_dir/nested.jsonl
--remove_file $file_sql_dir/export.csv
--rmdir $file_sql_dir
SET enable_file_sql=OFF;
