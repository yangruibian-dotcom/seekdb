"""Generate real Parquet fixtures; compare the native format reader with Arrow.

Arrow is a test-data generator/oracle only. Production has no Arrow dependency.
Usage: python test_file_parquet.py /path/to/test_file_scan_utils
"""
import datetime as dt
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import pyarrow as pa
import pyarrow.parquet as pq

exe = Path(sys.argv[1]).resolve()
checks = 0
def run(path, root, columns=None, ok=True):
    global checks
    args = [str(exe), str(path), 'parquet', str(root)]
    if columns is not None:
        args.append(','.join(map(str, columns)))
    p = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', timeout=30)
    checks += 1
    assert (p.returncode == 0) == ok, (path.name, p.stdout, p.stderr)
    return [line.rstrip('\t').split('\t') for line in p.stdout.splitlines()]

with tempfile.TemporaryDirectory(prefix='seekdb-parquet-') as temporary:
    root=Path(temporary)
    values={
        'id': pa.array([1, 2, None, -9223372036854775808, 9223372036854775807, 6, 7], type=pa.int64()),
        'name': pa.array(['alpha', '中文', None, 'alpha', '', 'beta', 'alpha']),
        'flag': pa.array([True, False, None, True, False, True, False]),
        'value': pa.array([1.5, None, 0., -2.25, 1e100, 6., 7.]),
        'day': pa.array([dt.date(1969,12,31),dt.date(1970,1,1),None,dt.date(2000,2,29),dt.date(2024,1,1),dt.date(2024,2,29),dt.date(2025,1,1)],type=pa.date32()),
        'stamp': pa.array([dt.datetime(1969,12,31,23,59,59,999999),dt.datetime(1970,1,1),None,dt.datetime(2000,2,29),dt.datetime(2024,1,1),dt.datetime(2024,2,29),dt.datetime(2025,1,1)],type=pa.timestamp('us')),
    }
    table=pa.table(values)
    for dictionary in (True, False):
        for compression in ('NONE', 'snappy'):
            for version in ('1.0', '2.0'):
                path=root/f'{dictionary}-{compression}-{version}.parquet'
                pq.write_table(table,path,row_group_size=3,use_dictionary=dictionary,compression=compression,data_page_version=version)
                result=run(path,root)
                assert result[0]==['id:BIGINT','name:VARCHAR','flag:BOOLEAN','value:DOUBLE','day:DATE','stamp:DATETIME'], result[0]
                assert len(result)==8
                for i,row in enumerate(result[1:]):
                    expected=[column[i].as_py() for column in values.values()]
                    for j,(got,want) in enumerate(zip(row,expected)):
                        if want is None: assert got=='<NULL>',(i,j,got)
                        elif isinstance(want,bool): assert got==str(want).lower()
                        elif isinstance(want,float): assert float(got)==want
                        elif isinstance(want,dt.datetime): assert dt.datetime.fromisoformat(got)==want
                        else: assert got==str(want),(i,j,got,want)
                        checks+=1
    # More than one data page in one row group; exercise all Snappy copy forms.
    many=pa.table({'id':range(12000),'text':['repeat-'+'abc'*40+str(i%7) for i in range(12000)]})
    path=root/'many-pages.parquet'
    pq.write_table(many,path,row_group_size=12000,data_page_size=1024,write_batch_size=128,use_dictionary=False,compression='snappy')
    rows=run(path,root)
    assert len(rows)==12001 and rows[-1][0]=='11999'
    # An unselected corrupt column is never read. Reading it must fail.
    original=bytearray(path.read_bytes())
    column=pq.ParquetFile(path).metadata.row_group(0).column(1)
    original[column.data_page_offset]=255
    corrupt=root/'prune.parquet';corrupt.write_bytes(original)
    assert len(run(corrupt,root,[0]))==12001
    run(corrupt,root,ok=False)
    nested=root/'nested.parquet';pq.write_table(pa.table({'x':[[1,2],None]}),nested)
    run(nested,root,ok=False)
    # A tiny index page can expand into gigabytes if dictionary copies are not
    # charged before allocation. Arrow stores this fixture as one dictionary.
    repeated=pa.DictionaryArray.from_arrays(pa.array([0]*4096,type=pa.int32()),pa.array(['x'*(512*1024)]))
    bomb=root/'dictionary-expansion.parquet'
    pq.write_table(pa.table({'text':repeated}),bomb,compression='snappy',write_statistics=False)
    run(bomb,root,ok=False)
    for payload in (b'',b'PAR1',b'PAR1'+b'\xff'*8):
        broken=root/'broken.parquet';broken.write_bytes(payload);run(broken,root,ok=False)
    nulls=pa.table({'i':pa.array([None]*4,type=pa.int64()),'b':pa.array([None]*4,type=pa.bool_()),'s':pa.array([None]*4,type=pa.string())})
    for version in ('1.0','2.0'):
        nullpath=root/f'all-null-{version}.parquet'
        pq.write_table(nulls,nullpath,row_group_size=2,data_page_version=version)
        assert all(row==['<NULL>']*3 for row in run(nullpath,root)[1:])
    # Bounded mutation tests: malformed metadata/pages must return or fail,
    # never terminate abnormally or hang. Successful mutations may be valid.
    rng=random.Random(451)
    data=path.read_bytes()
    for n in range(24):
        fuzz=bytearray(data);at=rng.randrange(len(fuzz));fuzz[at]^=1<<rng.randrange(8)
        broken=root/'mutation.parquet';broken.write_bytes(fuzz)
        p=subprocess.run([str(exe),str(broken),'parquet',str(root)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,timeout=30)
        assert p.returncode in (0,1), (n,p.returncode,p.stderr)
        checks+=1
print(json.dumps({'parquet_checks':checks,'status':'PASS'}))
