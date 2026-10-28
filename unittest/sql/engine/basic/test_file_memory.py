"""Check that schema inference + scanning do not retain an entire 100 MB CSV."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

exe=Path(sys.argv[1]).resolve()
def peak_bytes(process):
    if os.name=='nt':
        from ctypes import wintypes
        class Counters(ctypes.Structure):
            _fields_=[('cb',wintypes.DWORD),('faults',wintypes.DWORD)]+[(name,ctypes.c_size_t) for name in
              ('peak_ws','working_set','peak_paged','paged','peak_nonpaged','nonpaged','pagefile','peak_pagefile')]
        counters=Counters();counters.cb=ctypes.sizeof(counters)
        f=ctypes.WinDLL('psapi').GetProcessMemoryInfo
        f.argtypes=[wintypes.HANDLE,ctypes.POINTER(Counters),wintypes.DWORD]
        return counters.peak_ws if f(int(process._handle),ctypes.byref(counters),counters.cb) else 0
    try:
        status=Path(f'/proc/{process.pid}/status').read_text()
        return int(next(line.split()[1] for line in status.splitlines() if line.startswith('VmHWM:')))*1024
    except (OSError,StopIteration):
        return 0

with tempfile.TemporaryDirectory(prefix='seekdb-memory-') as temporary:
    root=Path(temporary);path=root/'stream.csv'
    line=b'1,'+b'x'*2048+b'\n'
    with path.open('wb') as f:
        f.write(b'id,text\n')
        for _ in range(100): f.write(line*512)
    with (root/'stderr').open('wb') as err:
        p=subprocess.Popen([str(exe),str(path),'csv',str(root)],stdout=subprocess.DEVNULL,stderr=err)
        peak=0;start=time.monotonic()
        while p.poll() is None:
            peak=max(peak,peak_bytes(p))
            if time.monotonic()-start>180:
                p.kill();raise AssertionError('streaming test timed out')
            time.sleep(.025)
        assert p.returncode==0,(root/'stderr').read_text()
    assert peak>0,'memory measurement unavailable'
    assert peak<64*1024*1024,peak
    print(json.dumps({'status':'PASS','file_bytes':path.stat().st_size,'peak_process_bytes':peak,'limit_bytes':64*1024*1024}))
