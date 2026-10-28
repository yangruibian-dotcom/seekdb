# 本地文件 SQL 实现说明

基线：`yangruibian-dotcom/seekdb`，commit `f2b8b0c909a7057ce3a61f145cb6c494048a2d75`。

**交付状态：实现候选，尚未完成整库构建和服务端验收。** 格式扫描模块及生成的 C 解析器已经编译验证；原生 SQL、JOIN、DESCRIBE、COPY 的接入代码需要在完整 seekdb 构建中继续验证。不要将独立格式测试的通过视为全部评分项通过。

## 使用接口

管理员先将 `secure_file_priv` 配置为已有的专用本地目录。功能默认关闭，每个使用文件 SQL 的会话需要开启：

```sql
SET enable_file_sql = ON;

SELECT id, amount * 2 AS doubled
FROM read_csv('/data/users.csv', header=true, delimiter=',', sample_size=-1) AS u
WHERE amount > 0
ORDER BY doubled DESC LIMIT 10;

SELECT team, COUNT(*), SUM(amount), AVG(amount), MIN(amount), MAX(amount)
FROM read_csv('/data/users.csv')
GROUP BY team HAVING COUNT(*) > 1;

SELECT u.id, t.label
FROM read_csv('/data/users.csv') u
LEFT JOIN read_json('/data/teams.jsonl') t ON u.team=t.team;

SELECT * FROM read_ndjson('/data/teams.jsonl');
SELECT id FROM read_parquet('/data/numbers.parquet');
SELECT file FROM glob('/data/*.[cj]*');
DESCRIBE SELECT * FROM read_csv('/data/users.csv');

COPY (SELECT id, name, amount FROM read_csv('/data/users.csv') ORDER BY id)
TO '/data/export.csv'
(FORMAT CSV, HEADER, DELIMITER ';', QUOTE '"', ESCAPE '"', NULL 'NA');

SELECT * FROM read_csv('/data/export.csv', delimiter=';', nullstr='NA');
COPY ordinary_table TO '/data/table.csv' (HEADER false);
```

路径和选项接受字面量。CSV 支持 `header`、`delimiter`（别名 `delim`、`sep`）、`quote`、`escape`、`nullstr`（别名 `null_string`），CSV/JSONL 的 `sample_size` 只接受 `-1`。选项名不区分 ASCII 大小写，重复和未知选项报错。改变 quote 且未指定 escape 时，两者采用相同字符。

`glob` 只产生一列 `file`，按完整路径排序；支持路径组件中的 `*`、`?`、`[abc]`、`[a-z]`，不支持 `**` 递归。文件读取函数不会将 glob 结果自动合并。

## 执行链路

1. MySQL 词法器和 Bison 语法新增文件表函数及 COPY 节点。文件函数名和 copy 保持非保留关键字，普通列名可继续使用这些名称。
2. Resolver 在访问校验后完成全量 CSV/JSONL Schema 推断；Parquet 从 Footer 获取 Schema。推断期间只保留当前记录、列结构及必要的格式元数据。
3. 文件表复用原生 `FUNCTION TABLE` 逻辑算子和物理算子，列引用保留原始 column id。文件描述符保存到计划常量，由算子在 open 时校验并打开只读输入。
4. 表达式、WHERE、聚合、HAVING、排序、LIMIT、INNER/LEFT JOIN 由 seekdb 原生计划执行。文件模块只负责格式解码，没有 SQL 执行器。
5. Parquet 根据实际引用的 column id 裁剪读取列。COUNT(*) 可使用 Footer 的行数，不读取未使用的列数据页。
6. 文件查询关闭计划缓存并限制并行度为 1；重扫重置格式状态并再次校验授权和文件身份。
7. DESCRIBE 解析源 SELECT 后生成列元数据常量查询，不执行源查询的数据阶段。普通 DESCRIBE/EXPLAIN 路径继续保留。
8. COPY 复用原生 SELECT INTO 计划和现有 FILE 权限检查，文件写入器支持行执行和批执行。完成查询后发布目标文件，异常时删除临时文件。

生产依赖使用仓库已有的 RapidJSON、fast_float 和 C++ 标准库。测试中的 PyArrow 仅用于生成及核对 Parquet 夹具。

## 类型与格式边界

支持逻辑类型 NULL、BIGINT、DOUBLE、BOOLEAN、VARCHAR、DATE、DATETIME。MySQL 协议下 BOOLEAN 使用引擎的 TINYINT 表示。

宽化规则：NULL 与其他类型合并采用其他类型；BIGINT + DOUBLE 为 DOUBLE；DATE + DATETIME 为 DATETIME；其余冲突为 VARCHAR。全 NULL 列保留 NULL 类型。整数超出有符号 64 位范围时保留原始文本，JSON 数字使用 SAX 的原始数字事件，避免先经过浮点转换。

日期严格校验公历合法性，年份范围 1–9999；DATETIME 支持空格或 T 分隔和最多 6 位小数。无效日期参与 VARCHAR 推断。JSONL 支持扁平对象、字段并集、缺失字段补 NULL；拒绝重复键、嵌套对象、数组和非法 UTF-8。

CSV 支持 UTF-8 BOM、普通 UTF-8、LF/CRLF、字段内换行、双引号转义、独立 escape 字符、空字段 NULL 和自定义 NULL 文本。带引号的空字符串及 NULL 文本保留为非 NULL 值。错误包含文件、记录号和记录起始物理行，JSON 还包含行内字节位置。

Parquet 支持扁平 Required/Optional 列、多 Row Group、多数据页、INT32/INT64/INT96、FLOAT/DOUBLE、BOOLEAN、UTF-8 字节数组、DATE、毫秒/微秒时间戳及可精确转换为微秒的纳秒时间戳。支持 PLAIN、PLAIN/RLE Dictionary、RLE/bit-packing、V1/V2 数据页、无压缩及 Snappy；存在 CRC 时进行校验。

当前明确拒绝 Gzip/Zstd 等其他压缩 codec、DELTA_*、BYTE_STREAM_SPLIT、无符号整数注解、Decimal、嵌套/重复结构、远程文件和外部列块。未支持的格式特性返回诊断，不代用其他 SQL 引擎。

## 安全和资源约束

- 默认关闭 enable_file_sql；解析及执行时均检查开关和运行时全局 secure_file_priv。
- 输入只读；路径按目录组件核验，拒绝越界、非普通文件、NUL 和远程路径。
- POSIX 使用 openat/O_NOFOLLOW 逐组件打开目录和文件；Windows 固定目录句柄、拒绝 reparse point、设备路径和 ADS，输入句柄禁止并发写入和删除。
- 文件身份包括设备、文件 id、长度、mtime、ctime/ChangeTime，推断结束、执行打开、逐记录和重扫检查变化。
- 默认单记录 16 MiB、4096 列、文件 1 TiB、Footer 16 MiB、Parquet 页 32 MiB、解码缓存 96 MiB、glob 10000 项。
- 批次最多 4096 行，并以约 8 MiB 累积数据作为提前返回阈值；一条大记录可单独超过该批次阈值，但仍受记录限额约束。
- 字典展开前计算缓存预算，防止一个小索引页复制巨大字典值而耗尽内存。
- COPY 在目标同目录创建排他临时文件；已存在目标失败。POSIX linkat 和 Windows MoveFile 均不覆盖目标。查询完成前不发布目标，失败或析构清理临时文件。

## 复现验证

完整环境：

```bash
python3 build.py init
python3 build.py build -j 4 //:seekdb
python3 build.py test -j 4 \
  //unittest/sql/engine/basic:test_file_scan_utils \
  //unittest/sql/parser:test_file_sql_parser
```

独立格式测试无需启动数据库；以下 include 参数分别指向含 rapidjson/ 和 fast_float/ 子目录的目录：

```bash
clang++ -std=c++20 -O1 -I src -I RAPIDJSON_INCLUDE -I FAST_FLOAT_INCLUDE \
  src/sql/engine/basic/file_sql.cpp \
  src/sql/engine/basic/file_sql_parquet.cpp \
  unittest/sql/engine/basic/test_file_scan_utils.cpp \
  -o test_file_scan_utils

python3 -m pip install pyarrow
python3 unittest/sql/engine/basic/test_file_parquet.py ./test_file_scan_utils
python3 unittest/sql/engine/basic/test_file_memory.py ./test_file_scan_utils
```

服务启动后，先创建专用测试目录，并由管理员配置对应的 GLOBAL secure_file_priv，再执行带结果断言的原生 SQL 联调：

```bash
python3 unittest/sql/engine/basic/test_file_sql_integration.py \
  --mysql mysql --host 127.0.0.1 --port 2881 --user root \
  --database test --secure-directory /tmp/seekdb_file_sql_mysqltest
```

密码可由 MYSQL_PWD 提供。脚本不修改 GLOBAL secure_file_priv，在其子目录部署夹具；它验证过滤、聚合、Schema、三种格式、原生 JOIN 计划、跨格式 JOIN、越界拒绝以及 COPY 读回和异常清理。

也提供了题面路径下的 mysqltest 脚本和部署/清理 include。由于尚未运行完整服务，本次没有生成或伪造 mysqltest 的 .result 文件。

Parquet 格式依据：[Apache Parquet 编码规范](https://parquet.apache.org/docs/file-format/data-pages/encodings/)、[parquet.thrift](https://github.com/apache/parquet-format/blob/master/src/main/thrift/parquet.thrift)。
