"""Native server regressions; PyMySQL is a protocol client, never an SQL engine.

Also executes the SQL bodies of the unchanged official file_sql_basic.test and
compares its logged result with file_sql_basic.result. Fixture deployment is
local instead of the official SSH wrapper. Run with --port and --directory.
"""
import argparse
import base64
import datetime
import decimal
import difflib
import json
from pathlib import Path
import subprocess
import sys


def literal(value):
    return "'" + str(value).replace('\\', '/').replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    import pymysql
    repo = Path(__file__).resolve().parents[4]
    root = args.directory.resolve()
    root.mkdir(exist_ok=True)
    args.output.mkdir(exist_ok=True)
    connection = pymysql.connect(host='127.0.0.1', port=args.port, user='root',
                                 charset='utf8mb4', autocommit=True, read_timeout=90)
    cursor = connection.cursor()
    checks = 0

    def query(sql):
        nonlocal checks
        checks += 1
        print('SQL:', sql, flush=True)
        cursor.execute(sql)
        rows = cursor.fetchall()
        print('RESULT:', repr(rows), flush=True)
        return rows

    def error(sql, text=None, code=None):
        try:
            query(sql)
        except pymysql.MySQLError as failure:
            if text:
                assert text.lower() in str(failure).lower(), str(failure)
            if code:
                assert failure.args[0] == code, (failure, code)
            return
        raise AssertionError('Expected failure: ' + sql)

    try:
        query('CREATE DATABASE IF NOT EXISTS test')
        query('USE test')
        configured = query('SELECT @@global.secure_file_priv')[0][0]
        assert Path(configured).resolve() == root, 'configure GLOBAL secure_file_priv through the local endpoint before running'
        assert query('SELECT @@session.enable_file_sql') == ((0,),)
        suite = 'tools/deploy/mysql_test/test_suite/file_sql/'
        official = root/'official'
        official.mkdir(exist_ok=True)
        for name in ('basic.csv', 'basic.jsonl', 'invalid.txt', 'simple.parquet.b64'):
            data = subprocess.check_output(['git', '-C', str(repo), 'show', 'HEAD:'+suite+'data/'+name])
            if name.endswith('.b64'):
                (official/'simple.parquet').write_bytes(base64.b64decode(data))
            else:
                (official/name).write_bytes(data)
        script = subprocess.check_output(['git','-C',str(repo),'show','HEAD:'+suite+'t/file_sql_basic.test']).decode()
        expected = subprocess.check_output(['git','-C',str(repo),'show','HEAD:'+suite+'r/mysql/file_sql_basic.result']).decode()
        transcript = []
        logging = True
        expected_error = None
        pending = ''

        def cell(value):
            if value is None:
                return 'NULL'
            if isinstance(value, float):
                return format(value, '.15g')
            if isinstance(value, bytes):
                return value.decode('utf8')
            return str(value)

        for line in script.splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith('#') or stripped.startswith('let ') or stripped.startswith('--source'):
                continue
            if stripped == '--disable_result_log':
                logging = False
                continue
            if stripped == '--enable_result_log':
                logging = True
                continue
            if stripped.startswith('--echo '):
                transcript.append(stripped[7:])
                continue
            if stripped.startswith('--error '):
                expected_error = int(stripped.split()[1])
                continue
            if stripped.startswith('--'):
                continue
            pending += ' ' + stripped
            if not stripped.endswith(';'):
                continue
            sql = pending.strip().removeprefix('eval ').replace('$FILE_SQL_DATA_DIR', str(official).replace('\\','/'))
            pending = ''
            if expected_error:
                error(sql, code=expected_error)
                expected_error = None
                continue
            rows = query(sql)
            if logging and cursor.description:
                transcript.append('\t'.join(d[0] for d in cursor.description))
                transcript.extend('\t'.join(cell(v) for v in row) for row in rows)
        actual = '\n'.join(transcript) + '\n'
        (args.output/'official-basic-actual.txt').write_text(actual, encoding='utf8')
        (args.output/'official-basic-expected.txt').write_text(expected, encoding='utf8')
        difference = ''.join(difflib.unified_diff(expected.splitlines(True),actual.splitlines(True)))
        (args.output/'official-basic.diff').write_text(difference, encoding='utf8')
        assert not difference, difference
        official_checks = checks
        print('PASS: unchanged official SQL bodies and expected results', flush=True)

        query('SET enable_file_sql=ON')
        data = root/'candidate'
        data.mkdir(exist_ok=True)
        (data/'users.csv').write_text('id,name,team,amount,active\n1,Alice,A,10.5,true\n2,Bob,A,20,false\n3,Chen,B,5.5,true\n4,Dan,B,,true\n',encoding='utf8')
        (data/'teams.jsonl').write_text('{"team":"A","label":"Alpha"}\n{"team":"B","label":"Beta"}\n{"team":"C","label":"Gamma"}\n',encoding='utf8')
        csv = 'read_csv('+literal(data/'users.csv')+')'
        js = 'read_ndjson('+literal(data/'teams.jsonl')+')'
        assert query('SELECT id,amount*2 AS doubled FROM '+csv+' u WHERE active=true AND amount IS NOT NULL ORDER BY id DESC LIMIT 1') == ((3,11.0),)
        grouped = query('SELECT team,COUNT(*),COUNT(amount),SUM(amount),AVG(amount),MIN(amount),MAX(amount) FROM '+csv+' GROUP BY team HAVING COUNT(*)>1 ORDER BY team')
        assert grouped[0][:3] == ('A',2,2) and [float(v) for v in grouped[0][3:]] == [30.5,15.25,10.5,20]
        assert grouped[1][:3] == ('B',2,1)
        assert query('SELECT COUNT(*) FROM '+csv+' u INNER JOIN '+js+' t ON u.team=t.team') == ((4,),)
        assert query('SELECT t.team,COUNT(u.id) FROM '+js+' t LEFT JOIN '+csv+' u ON t.team=u.team GROUP BY t.team ORDER BY t.team') == (('A',2),('B',2),('C',0))
        assert query('SELECT /*+ USE_NL(t) */ COUNT(*) FROM '+csv+' u LEFT JOIN '+js+' t ON u.team=t.team') == ((4,),)
        rescan_sql='SELECT COUNT(*) FROM '+csv+' u LEFT JOIN '+js+' t ON u.team<t.team'
        assert query(rescan_sql) == ((6,),)
        rescan_plan='\n'.join(str(row[0]) for row in query('EXPLAIN '+rescan_sql))
        assert 'NESTED-LOOP' in rescan_plan.upper(),rescan_plan
        (args.output/'native-rescan-plan.txt').write_text(rescan_plan+'\n',encoding='utf8')
        parquet = 'read_parquet('+literal(official/'simple.parquet')+')'
        assert query('SELECT p.c1,u.name FROM '+parquet+' p LEFT JOIN '+csv+' u ON p.c1=u.id WHERE p.c1<=2 ORDER BY p.c1') == ((0,None),(1,'Alice'),(2,'Bob'))
        # PyArrow creates a fixture; every query below runs inside seekdb.
        import pyarrow as pa
        import pyarrow.parquet as pq
        typed=data/'typed.parquet'
        pq.write_table(pa.table({
            'id':pa.array([1,2,None,5],type=pa.int64()),
            'name':['中文','two',None,'five'],
            'flag':[True,False,None,True],
            'value':[1.5,-2.25,None,3.0],
            'day':pa.array([datetime.date(1969,12,31),datetime.date(2024,2,29),None,datetime.date(2000,1,1)],type=pa.date32()),
            'stamp':pa.array([datetime.datetime(1969,12,31,23,59,59,123456),datetime.datetime(2024,2,29,1,2,3,4),None,datetime.datetime(2000,1,1)],type=pa.timestamp('us')),
        }),typed,row_group_size=2,compression='snappy',data_page_version='2.0')
        typed_sql='read_parquet('+literal(typed)+')'
        assert query('SELECT COUNT(*),COUNT(name),SUM(value) FROM '+typed_sql) == ((4,3,2.25),)
        typed_rows=query('SELECT * FROM '+typed_sql+' WHERE id IS NOT NULL ORDER BY id')
        assert typed_rows[0] == (1,'中文',1,1.5,datetime.date(1969,12,31),datetime.datetime(1969,12,31,23,59,59,123456)),typed_rows
        assert typed_rows[1][-1] == datetime.datetime(2024,2,29,1,2,3,4),typed_rows
        plan = ' '.join(str(row) for row in query('EXPLAIN SELECT u.id FROM '+csv+' u JOIN '+js+' t ON u.team=t.team')).upper().replace('_',' ')
        assert 'JOIN' in plan and 'FUNCTION TABLE' in plan, plan
        (args.output/'native-join-plan.txt').write_text(plan+'\n',encoding='utf8')
        query('CREATE TABLE ordinary_file_sql_test (id INT, label VARCHAR(30))')
        query("INSERT INTO ordinary_file_sql_test VALUES (1,'ok'),(2,'fine')")
        assert query('SELECT COUNT(*) FROM ordinary_file_sql_test') == ((2,),)
        target=data/'table-export.csv'
        query('COPY ordinary_file_sql_test TO '+literal(target)+" (FORMAT CSV,HEADER,DELIMITER ';',NULLSTR 'NA')")
        assert query('SELECT COUNT(*) FROM read_csv('+literal(target)+",delimiter=';',nullstr='NA')") == ((2,),)
        error('COPY ordinary_file_sql_test TO '+literal(target), 'exists')
        export = data/'roundtrip.csv'
        query('COPY (SELECT id,name,amount FROM '+csv+' ORDER BY id) TO '+literal(export)+" (FORMAT CSV,HEADER,DELIMITER ';',NULLSTR 'NA')")
        assert query('SELECT COUNT(*),COUNT(amount),SUM(amount) FROM read_csv('+literal(export)+",delimiter=';',nullstr='NA')") == ((4,3,36.0),)
        query('CREATE TABLE copy_payloads (id INT, body LONGTEXT, precise DOUBLE, stamp DATETIME(6))')
        query("INSERT INTO copy_payloads VALUES (1,CONCAT('left',REPEAT('x',70000),'right'),1.2345678901234567,'2024-02-29 01:02:03.123456')")
        query("INSERT INTO copy_payloads VALUES (2,NULL,NULL,NULL),(3,'',0,'2000-01-01'),(4,'中文',-1e-200,'1969-12-31 23:59:59.000001')")
        payload_export=data/'payload-export.csv'
        query('COPY copy_payloads TO '+literal(payload_export)+' (HEADER)')
        payload_rows=query('SELECT id,LENGTH(body),LEFT(body,4),RIGHT(body,5),precise,stamp FROM read_csv('+literal(payload_export)+') ORDER BY id')
        assert payload_rows == (
            (1,70009,'left','right',1.2345678901234567,datetime.datetime(2024,2,29,1,2,3,123456)),
            (2,None,None,None,None,None),
            (3,0,'','',0.0,datetime.datetime(2000,1,1)),
            (4,6,'中文','中文',-1e-200,datetime.datetime(1969,12,31,23,59,59,1))),payload_rows
        nul_export=data/'nul-export.csv'
        error("COPY (SELECT CONCAT('left',CHAR(0),'right') AS body) TO "+literal(nul_export),'NUL')
        assert not nul_export.exists() and not list(data.glob('*.seekdb-*')),'invalid CSV output leaked a file'
        invalid_export=data/'invalid-utf8-export.csv'
        error('COPY (SELECT CAST(0xC080 AS BINARY) AS body) TO '+literal(invalid_export),'UTF-8')
        assert not invalid_export.exists() and not list(data.glob('*.seekdb-*')),'invalid UTF-8 output leaked a file'
        query('DROP TABLE copy_payloads')
        special=data/'中文.csv'
        special.write_bytes('name,value\r\n"x;\"\"quoted\"\"","line1\r\nline2"\r\n"NULL",\r\n'.encode('utf8'))
        special_sql='read_csv('+literal(special)+')'
        special_rows=query('SELECT * FROM '+special_sql+' ORDER BY name')
        special_export=data/'special-export.csv'
        query('COPY (SELECT * FROM '+special_sql+' ORDER BY name) TO '+literal(special_export)+" (FORMAT CSV,HEADER,DELIMITER ';',QUOTE '~',ESCAPE '!',NULLSTR 'NULL')")
        assert query('SELECT * FROM read_csv('+literal(special_export)+",delimiter=';',quote='~',escape='!',nullstr='NULL') ORDER BY name") == special_rows
        empty_export=data/'empty.csv'
        query('COPY (SELECT id,label FROM ordinary_file_sql_test WHERE id<0) TO '+literal(empty_export)+' (HEADER)')
        assert query('SELECT COUNT(*) FROM read_csv('+literal(empty_export)+')') == ((0,),)
        no_header=data/'no-header.csv'
        no_header.write_bytes(b'1;alpha\r\n2;beta\r\n')
        assert query('SELECT column1,column2 FROM read_csv('+literal(no_header)+",header=false,delimiter=';') ORDER BY column1") == ((1,'alpha'),(2,'beta'))
        union=data/'union.jsonl'
        union.write_text('{"id":1,"v":1,"date":"2024-02-29"}\n{"id":2,"v":1.5,"later":true}\n{"id":3,"date":"bad-date","later":null}\n',encoding='utf8')
        assert query('SELECT id,v,date,later FROM read_json('+literal(union)+') ORDER BY id') == ((1,1.0,'2024-02-29',None),(2,1.5,None,1),(3,None,'bad-date',None))
        metadata=query('DESCRIBE SELECT * FROM read_json('+literal(union)+')')
        assert [(r[0],r[1],r[2]) for r in metadata] == [('id','BIGINT','NO'),('v','DOUBLE','YES'),('date','VARCHAR','YES'),('later','BOOLEAN','YES')],metadata
        many=data/'batch.csv'
        many.write_text('v\n'+''.join(str(i)+'\n' for i in range(10001)),encoding='utf8')
        assert query('SELECT COUNT(*),SUM(v),MIN(v),MAX(v) FROM read_csv('+literal(many)+')') == ((10001,50005000,0,10000),)
        assert query('SELECT COUNT(*) FROM '+csv+' u JOIN read_csv('+literal(many)+') b ON u.id=b.v') == ((4,),)
        (data/'types.csv').write_text('big,over,flag,day,stamp,empty\n-9223372036854775808,9223372036854775808,True,1969-12-31,1969-12-31T23:59:59.123456,\n9223372036854775807,18446744073709551616,fAlSe,2024-02-29,2024-02-29 01:02:03.000004,\n',encoding='utf8')
        types='read_csv('+literal(data/'types.csv')+')'
        values=query('SELECT * FROM '+types+' ORDER BY big')
        assert values[0] == (-9223372036854775808,'9223372036854775808',1,datetime.date(1969,12,31),datetime.datetime(1969,12,31,23,59,59,123456),None),values
        assert values[1][-2] == datetime.datetime(2024,2,29,1,2,3,4),values
        assert query('SELECT COUNT(empty) FROM '+types) == ((0,),)
        (data/'late.csv').write_text('v\n'+'1\n'*260+'1.5\n',encoding='utf8')
        assert query('SELECT COUNT(*),SUM(v) FROM read_csv('+literal(data/'late.csv')+')') == ((261,261.5),)
        changed=data/'changed.csv'
        changed.write_text('v\n1\n',encoding='utf8')
        change_sql='SELECT COUNT(*) FROM read_csv('+literal(changed)+')'
        assert query(change_sql) == ((1,),)
        changed.write_text('v\n1\n2\n',encoding='utf8')
        assert query(change_sql) == ((2,),)
        query('PREPARE file_candidate_ps FROM '+literal(change_sql))
        assert query('EXECUTE file_candidate_ps') == ((2,),)
        changed.write_text('v\n1\n2\n3\n',encoding='utf8')
        try:
            assert query('EXECUTE file_candidate_ps') == ((3,),)
        except pymysql.MySQLError as failure:
            assert 'changed' in str(failure).lower(),failure
        query('DEALLOCATE PREPARE file_candidate_ps')
        broken=data/'broken.parquet'
        raw=bytearray((official/'simple.parquet').read_bytes())
        raw[4]=255
        broken.write_bytes(raw)
        failed=data/'failed.csv'
        error('COPY (SELECT * FROM read_parquet('+literal(broken)+')) TO '+literal(failed),'Parquet')
        assert not failed.exists() and not list(data.glob('*.seekdb-*')),'failed COPY leaked a file'
        (data/'nested.jsonl').write_text('{"a":{"b":1}}\n',encoding='utf8')
        error('SELECT * FROM read_json('+literal(data/'nested.jsonl')+')','nested')
        (data/'bad.csv').write_text('a,b\n1\n',encoding='utf8')
        error('SELECT * FROM read_csv('+literal(data/'bad.csv')+')','record')
        error('SELECT * FROM '+csv[:-1]+',sample_size=1)','sample_size')
        error('SELECT * FROM '+csv[:-1]+',unknown_option=true)','unknown option')
        error('SELECT * FROM '+csv[:-1]+",delimiter=',',sep=';')",'duplicate option')
        outside=root.parent/'file-sql-outside.csv'
        outside.write_text('id\n1\n',encoding='utf8')
        try:
            error('SELECT * FROM read_csv('+literal(outside)+')','secure_file_priv')
            error('SELECT * FROM read_csv('+literal(root/'candidate/../../file-sql-outside.csv')+')','secure_file_priv')
            error('SELECT * FROM glob('+literal(root.parent/'*.csv')+')','secure_file_priv')
            error('COPY ordinary_file_sql_test TO '+literal(root.parent/'file-sql-escape.csv'),'secure_file_priv')
        finally:
            outside.unlink()
        query('SET enable_file_sql=OFF')
        error('SELECT * FROM '+csv,'disabled')
        assert query('SELECT id+2 FROM ordinary_file_sql_test ORDER BY id') == ((3,),(4,))
        query('DROP TABLE ordinary_file_sql_test')
        result={'status':'PASS','native_queries':checks,'official_and_setup_queries':official_checks,
                'official_sql_bodies':'PASS','official_mysqltest_ssh_wrapper':'NOT RUN'}
        (args.output/'native-sql-result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf8')
        print(json.dumps(result),flush=True)
    finally:
        connection.close()


if __name__ == '__main__':
    main()
