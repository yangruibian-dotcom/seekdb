"""Assertions against a locally built seekdb server via its MySQL protocol.

Configure GLOBAL secure_file_priv to --secure-directory before running.
MYSQL_PWD may supply a password; passwords are never put in command arguments.
No external SQL engine or Python database package is used.
"""
import argparse
from decimal import Decimal
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--mysql',default='mysql')
p.add_argument('--host',default='127.0.0.1')
p.add_argument('--port',type=int,default=2881)
p.add_argument('--user',default='root')
p.add_argument('--database',default='test')
p.add_argument('--secure-directory',type=Path,required=True)
a=p.parse_args()
root=a.secure_directory.resolve(strict=True)
checks=0
def literal(value):
    return "'"+str(value).replace('\\','/').replace("'","''")+"'"
def sql(query,enabled=True,error=None):
    global checks
    command=[a.mysql,'--host='+a.host,'--port='+str(a.port),'--user='+a.user,'--database='+a.database,
             '--batch','--skip-column-names','--default-character-set=utf8mb4','--connect-timeout=5']
    query=('SET enable_file_sql='+('ON' if enabled else 'OFF')+';\n')+query+';\n'
    r=subprocess.run(command,input=query,text=True,encoding='utf-8',capture_output=True,timeout=120)
    checks+=1
    if error is not None:
        assert r.returncode!=0,(query,'expected an error')
        assert error.lower() in r.stderr.lower(),(query,r.stderr)
        return []
    assert r.returncode==0,(query,r.stderr)
    return [line.split('\t') for line in r.stdout.splitlines()]

configured=sql('SELECT @@global.secure_file_priv')[0][0]
assert Path(configured).resolve()==root,'configure secure_file_priv to the exact test root first'
assert root!=root.parent,'use a dedicated directory instead of a filesystem root'
repo=Path(__file__).resolve().parents[4]
fixtures=repo/'tools/deploy/mysql_test/test_suite/file_sql/data'
with tempfile.TemporaryDirectory(prefix='file-sql-',dir=root) as d, tempfile.TemporaryDirectory(prefix='file-sql-outside-',dir=root.parent) as out:
    d=Path(d);out=Path(out)
    for name in ('users.csv','teams.jsonl','numbers.parquet','bad.csv','nested.jsonl'):
        shutil.copyfile(fixtures/name,d/name)
    csv='read_csv('+literal(d/'users.csv')+')'
    js='read_json('+literal(d/'teams.jsonl')+')'
    parquet='read_parquet('+literal(d/'numbers.parquet')+')'
    assert sql('SELECT 1+2')==[['3']]
    sql('SELECT * FROM '+csv,enabled=False,error='disabled')
    row=sql('SELECT id,amount*2 AS doubled FROM '+csv+' u WHERE active=true AND amount IS NOT NULL ORDER BY id DESC LIMIT 1')[0]
    assert row[0]=='3' and Decimal(row[1])==11
    grouped=sql('SELECT team,COUNT(*),COUNT(amount),SUM(amount),AVG(amount),MIN(amount),MAX(amount) FROM '+csv+' GROUP BY team HAVING COUNT(*)>1 ORDER BY team')
    assert grouped[0][:3]==['A','2','2'] and [Decimal(x) for x in grouped[0][3:]]==[Decimal('30.5'),Decimal('15.25'),Decimal('10.5'),Decimal('20')]
    assert grouped[1][:3]==['B','2','1']
    described=sql('DESCRIBE SELECT * FROM '+csv)
    assert [r[0] for r in described]==['id','name','team','amount','active']
    assert sql('SELECT COUNT(*) FROM '+js)==[['3']]
    assert sql('SELECT id FROM '+parquet+' ORDER BY id')==[['1'],['2'],['5']]
    assert sql('SELECT COUNT(*) FROM glob('+literal(d/'*.csv')+')')==[['2']]
    assert sql('SELECT COUNT(*) FROM '+csv+' u INNER JOIN '+js+' t ON u.team=t.team')==[['4']]
    assert sql('SELECT t.team,COUNT(u.id) FROM '+js+' t LEFT JOIN '+csv+' u ON t.team=u.team GROUP BY t.team ORDER BY t.team')==[['A','2'],['B','2'],['C','0']]
    assert sql('SELECT p.id,COALESCE(u.name,\'missing\') FROM '+parquet+' p LEFT JOIN '+csv+' u ON p.id=u.id ORDER BY p.id')==[['1','Alice'],['2','Bob'],['5','missing']]
    plan=' '.join(' '.join(row) for row in sql('EXPLAIN SELECT u.id FROM '+csv+' u JOIN '+js+' t ON u.team=t.team')).upper().replace('_',' ')
    assert 'JOIN' in plan and 'FUNCTION TABLE' in plan,plan
    export=d/'export.csv'
    sql('COPY (SELECT id,name,amount FROM '+csv+' ORDER BY id) TO '+literal(export)+" (FORMAT CSV, HEADER, DELIMITER ';', NULL 'NA')")
    back='read_csv('+literal(export)+",delimiter=';',nullstr='NA')"
    returned=sql('SELECT COUNT(*),COUNT(amount),SUM(amount) FROM '+back)[0]
    assert returned[:2]==['4','3'] and Decimal(returned[2])==36
    sql('COPY (SELECT 1 AS id) TO '+literal(export),error='exists')
    sql('SELECT * FROM read_csv('+literal(d/'bad.csv')+')',error='record')
    sql('SELECT * FROM read_json('+literal(d/'nested.jsonl')+')',error='nested')
    sql('SELECT * FROM read_csv('+literal(d/'users.csv')+',sample_size=1)',error='sample_size')
    escaped=out/'valid.csv';escaped.write_text('id\n1\n',encoding='utf-8')
    sql('SELECT * FROM read_csv('+literal(escaped)+')',error='secure_file_priv')
    corrupt=d/'corrupt.parquet';data=bytearray((d/'numbers.parquet').read_bytes());data[4]=255;corrupt.write_bytes(data)
    failed=d/'failed.csv'
    sql('COPY (SELECT * FROM read_parquet('+literal(corrupt)+')) TO '+literal(failed),error='Parquet')
    assert not failed.exists() and not list(d.glob('*.seekdb-*')),'failed COPY leaked files'
    assert sql('SELECT 1+2')==[['3']]
print(json.dumps({'status':'PASS','native_sql_queries':checks}))
