#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Compare full TOKENIZE output and ordered MATCH rows on two dedicated servers.

Set FTS_BASELINE_MYSQL_JSON and FTS_CANDIDATE_MYSQL_JSON to JSON arrays, e.g.
["mysql", "--defaults-extra-file=/private/baseline.cnf", "-h127.0.0.1", "-P2881"].
Credentials belong in the client option file. No shell is used or command logged.
Each instance gets a newly named database, removed in finally after creation.
This is correctness validation; it does not time or score competition metrics.
"""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import uuid


TEXTS = [
    None, '', 'hello', 'Hello HELLO world', 'database index search',
    '数据库 全文索引 中文检索', '数据库 database index 全文检索',
    'ＡＢＣ１２３ abc123 12.34 abc-def@example.com',
    '一二两三十百千万亿 三个人 123.45',
    '日本語 カタカナ 한국어 한글 😀 𠀀',
    ' \t\r\n...hello,,,world!!! ', 'hello\x00world',
    'hello world ' * 1024, '数据库 全文索引 性能优化。' * 128,
    'a' * 84 + ' ' + 'b' * 85, 'trailing punctuation!!!',
]


def sql_text(value):
    return 'NULL' if value is None else "CONVERT(X'%s' USING utf8mb4)" % value.encode('utf-8').hex()


def make_sql(database):
    statements = [f'USE `{database}`;', 'SET NAMES utf8mb4;', 'SET ob_query_timeout=10000000000;']
    modes = [
        ('ik_smart', 'ik', [{'additional_args': [{'ik_mode': 'smart'}]}]),
        ('ik_max', 'ik', [{'additional_args': [{'ik_mode': 'max_word'}]}]),
        ('beng', 'beng', []),
    ]
    for i, text in enumerate(TEXTS):
        for name, parser, properties in modes:
            for output in ('default', 'all'):
                props = sql_text(json.dumps([{'output': output}] + properties))
                tag = f'{name}_{output}_{i}'
                statements.append("SELECT JSON_OBJECT('kind','token','case','%s','result',TOKENIZE(%s,'%s',%s));" %
                                  (tag, sql_text(text), parser, props))
    for i, text in enumerate(('hello world', '中文 ＡＢＣ')):
        for parser in ('ik', 'beng'):
            statements.append("SELECT JSON_OBJECT('kind','token','case','utf16_%s_%d','result',TOKENIZE(CONVERT(%s USING utf16),'%s'));" %
                              (parser, i, sql_text(text), parser))
    statements.append('CREATE TABLE docs(id INT PRIMARY KEY, title VARCHAR(128), body TEXT, english TEXT);')
    for i, text in enumerate(TEXTS, 1):
        statements.append('INSERT INTO docs VALUES (%d,%s,%s,%s);' %
                          (i, sql_text('中国 database' if i % 2 else '世界 search'), sql_text(text), sql_text(text)))
    statements += [
        'CREATE FULLTEXT INDEX ft_ik_all ON docs(title,body) WITH PARSER ik;',
        'CREATE FULLTEXT INDEX ft_ik_content ON docs(body) WITH PARSER ik;',
        'CREATE FULLTEXT INDEX ft_beng ON docs(english) WITH PARSER beng;',
    ]
    def queries(stage):
        for n, (columns, term, mode, limit) in enumerate([
            ('title,body', '数据库', '', ''), ('english', 'database', '', ''),
            ('title,body', '数据库 database', '', ''), ('body', 'hello', '', ''),
            ('title,body', '+database +index', ' IN BOOLEAN MODE', ''),
            ('english', '"database index"', ' IN BOOLEAN MODE', ''),
            ('title,body', 'database', '', ' LIMIT 5'),
            ('body', 'definitelyabsenttokenzzzzz', '', ''),
        ]):
            match = 'MATCH(%s) AGAINST(%s%s)' % (columns, sql_text(term), mode)
            statements.append("SELECT JSON_OBJECT('kind','query','case','%s_%d','id',id,'score',%s) FROM docs WHERE %s > 0 ORDER BY %s DESC,id%s;" %
                              (stage, n, match, match, match, limit))
    queries('built')
    statements += [
        "INSERT INTO docs VALUES (100,'database','database index hello','database index hello');",
        "UPDATE docs SET body='database updated',english='database updated' WHERE id=4;",
        'DELETE FROM docs WHERE id=5;',
    ]
    queries('dml')
    statements += ['DROP INDEX ft_ik_all ON docs;', 'CREATE FULLTEXT INDEX ft_ik_all ON docs(title,body) WITH PARSER ik;']
    queries('rebuilt')
    return '\n'.join(statements) + '\n'


def run_client(command, sql):
    completed = subprocess.run(command + ['--batch', '--raw', '--skip-column-names', '--default-character-set=utf8mb4'],
                               input=sql, text=True, encoding='utf-8', capture_output=True, timeout=1800)
    if completed.returncode:
        # Avoid echoing client options or credentials from command/error text.
        raise RuntimeError('MySQL client failed with exit code %d; inspect the instance/client configuration.' % completed.returncode)
    return [json.loads(line) for line in completed.stdout.splitlines() if line.strip()]


def collect(command, database):
    run_client(command, f'CREATE DATABASE `{database}` CHARACTER SET utf8mb4;')
    try:
        return run_client(command, make_sql(database))
    finally:
        run_client(command, f'DROP DATABASE `{database}`;')


def canonical(row):
    row = dict(row)
    if row.get('kind') == 'token':
        value = row['result']
        if isinstance(value, list):
            row['result'] = sorted(value)
        elif isinstance(value, dict) and isinstance(value.get('tokens'), list):
            row['result'] = dict(value, tokens=sorted(value['tokens'], key=lambda item: json.dumps(item, sort_keys=True)))
    return row


def equal(left, right):
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        return math.isclose(left, right, rel_tol=1e-12, abs_tol=1e-12)
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(equal(left[k], right[k]) for k in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(equal(a, b) for a, b in zip(left, right))
    return left == right


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('work/fts-differential'))
    parser.add_argument('--emit-sql', type=Path, help='Generate SQL without connecting to a server.')
    args = parser.parse_args()
    if args.emit_sql:
        args.emit_sql.parent.mkdir(parents=True, exist_ok=True)
        args.emit_sql.write_text(make_sql('fts_hotpath_preview'), encoding='utf-8')
        return
    clients = []
    for name in ('FTS_BASELINE_MYSQL_JSON', 'FTS_CANDIDATE_MYSQL_JSON'):
        value = json.loads(os.environ[name])
        if not isinstance(value, list) or not value or any(not isinstance(v, str) for v in value):
            raise ValueError(name + ' must be a nonempty JSON array of strings')
        clients.append(value)
    database = 'fts_hotpath_' + uuid.uuid4().hex
    args.output.mkdir(parents=True, exist_ok=True)
    results = [list(map(canonical, collect(client, database))) for client in clients]
    for label, rows in zip(('baseline', 'candidate'), results):
        (args.output / (label + '.json')).write_text(json.dumps(rows, ensure_ascii=False, indent=2), encoding='utf-8')
    if not equal(*results):
        raise SystemExit('FAIL: TOKENIZE output or ordered MATCH rows/scores differ; inspect the saved JSON files.')
    print('PASS: %d result rows; complete token sets/frequencies/doc_len and ordered MATCH IDs/scores agree.' % len(results[0]))


if __name__ == '__main__':
    main()
