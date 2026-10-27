import ctypes
import dataclasses
import struct
import sys
import zlib
import argparse
import pathlib
import subprocess
import tempfile

parser=argparse.ArgumentParser(description="Actual firmware C core / broker Python wire interop; no hardware")
parser.add_argument("--broker-source", type=pathlib.Path, required=True)
args=parser.parse_args()
sys.path.insert(0,str(args.broker_source.resolve()/"src"))
from cardputer_mcp.app_wire import Request,Reply,Op,Result,State

root=pathlib.Path(__file__).resolve().parents[1]
temporary=tempfile.TemporaryDirectory(prefix="app-control-interop-")
library=pathlib.Path(temporary.name)/"interop.so"
subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-shared","-fPIC",
                "-I",str(root/"main/pocket"),str(root/"tools/test_app_control_interop.c"),
                str(root/"main/pocket/app_control.c"),"-o",str(library)],check=True)
lib=ctypes.CDLL(str(library))
lib.transact.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_ulonglong,ctypes.c_void_p]
lib.transact.restype=ctypes.c_uint
lib.reset()
now=0
def exchange(q):
    global now
    now+=110
    wire=q.encode()
    raw=bytes.fromhex(wire[2:-1].decode())
    assert zlib.crc32(raw[:-4])==struct.unpack('<I',raw[-4:])[0]
    out=ctypes.create_string_buffer(132)
    assert lib.transact(raw[:-4],len(raw)-4,now,out)>=1
    body=out.raw
    return Reply.decode(b'\x1dC'+(body+struct.pack('<I',zlib.crc32(body))).hex().encode()+b'\n')

cat=exchange(Request(Op.CATALOG,0,0))
assert cat.app_id=='local.hello' and cat.catalog_count==3
meta=exchange(Request(Op.CATALOG,0,0,seq=65535))
assert meta.app_id=='5a'*32 and meta.title=='interop-v1'
ctx=dict(session=cat.session,boot=cat.boot,fence=13)
assert exchange(Request(Op.OPEN,command_id=0,ttl=90000,**ctx)).result==Result.OK
assert exchange(Request(Op.START,command_id=1,seq=1,app_id='bad.id',**ctx)).result==Result.UNKNOWN_APP
start=Request(Op.START,command_id=2,seq=2,app_id='local.hello',**ctx)
r=exchange(start);assert r.result==Result.OK and r.state==State.RUNNING
run=r.run
assert exchange(start).run==run and lib.started()==1
assert exchange(dataclasses.replace(start,app_id='local.stress')).result==Result.CONFLICT
tap=Request(Op.TAP,command_id=3,run=run,seq=3,arg=1,**ctx)
assert exchange(tap).result==Result.CONSUMED
assert exchange(tap).result==Result.CONSUMED and lib.tapped()==1
assert exchange(dataclasses.replace(tap,op=Op.STATUS,app_id='',arg=0)).result==Result.CONSUMED
assert exchange(Request(Op.TAP,command_id=4,run=run+1,seq=4,arg=1,**ctx)).result==Result.STALE_RUN
assert exchange(Request(Op.SUSPEND,command_id=4,run=run,seq=4,**ctx)).state==State.SUSPENDED
r=exchange(Request(Op.START,command_id=5,run=run,seq=5,app_id='local.hello',arg=1,**ctx))
assert r.run==run and r.state==State.RUNNING and lib.started()==2
assert exchange(Request(Op.STOP,command_id=6,run=run,seq=6,**ctx)).state==State.STOPPED
assert exchange(Request(Op.CLOSE,command_id=7,run=run,seq=7,**ctx)).result==Result.OK
assert exchange(Request(Op.OPEN,command_id=0,ttl=90000,**ctx)).result==Result.STALE_SESSION
assert lib.tapped()==1
cat2=exchange(Request(Op.CATALOG,0,0))
assert cat2.session!=cat.session
assert exchange(Request(Op.STATUS,command_id=999,seq=999,run=run,**ctx)).result==Result.OUTCOME_UNKNOWN
print('PASS: broker Python wire <-> actual firmware C core: catalog/fingerprint, unknown ID, fresh/resume, suspend/stop, consumed/dedup/conflict, lost-ACK STATUS, stale run, retired OPEN, ledger miss; no replay')

# Exercise the production single-reader transport against that same C core,
# including a lost mutation ACK. The only recovery write must be STATUS.
from cardputer_mcp.app_transport import ControlConnection

lib.reset()
now=0
class CorePort:
    def __init__(self):
        self.buffer=b''
        self.writes=[]
        self.drop_tap_ack=True
    def write(self, wire):
        query=Request.decode(wire)
        self.writes.append(query)
        response=exchange(query).encode()
        if query.op==Op.TAP and self.drop_tap_ack:
            self.drop_tap_ack=False
        else:
            self.buffer+=response
        return len(wire)
    def read(self,size=1):
        data,self.buffer=self.buffer[:size],self.buffer[size:]
        return data

port=CorePort()
virtual=[0.0]
def pause(seconds):
    virtual[0]+=seconds
link=ControlConnection(port,clock=lambda:virtual[0],pause=pause,timeout=0.05)
items=link.catalog()
ctx=dict(boot=items[0].boot,fence=29)
session=items[0].session
assert link.send(Op.OPEN,session,0,ttl=90000,**ctx).result==Result.OK
started=link.send(Op.START,session,1,seq=1,app_id='local.stress',**ctx)
assert started.result==Result.OK
assert link.send(Op.TAP,session,2,run=started.run,seq=2,arg=1,**ctx).result==Result.CONSUMED
assert [q.op for q in port.writes[-2:]]==[Op.TAP,Op.STATUS]
assert lib.tapped()==1
assert link.send(Op.STOP,session,3,run=started.run,seq=3,**ctx).state==State.STOPPED
assert link.send(Op.CLOSE,session,4,run=started.run,seq=4,**ctx).result==Result.OK
print('PASS: production broker ControlConnection <-> actual C core; lost TAP ACK sends STATUS only, tap applied once, STOP/CLOSE confirmed')
