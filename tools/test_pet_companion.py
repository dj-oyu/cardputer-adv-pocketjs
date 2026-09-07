import json
from contextlib import closing
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
import pet_companion as p


class CompanionTest(unittest.TestCase):
    def test_bucket_and_unknown(self):
        rates={'rateLimitsByLimitId':{'other':{'primary':{'usedPercent':90,'resetsAt':1800000000}}}}
        a=p.normalize_codex(rates,{'summary':{'lifetimeTokens':None}})
        self.assertIsNone(a['tokens']);self.assertEqual(a['windows'],[None,None])
        rates['rateLimitsByLimitId']['codex']={'primary':{'usedPercent':0,'resetsAt':1800000000}}
        a=p.normalize_codex(rates,{'summary':{'lifetimeTokens':12000}})
        self.assertEqual(a['tokens'],12000);self.assertEqual(a['windows'][0]['usedPercent'],0)
        self.assertIsNone(p.window({'u':float('nan'),'r':1},'u','r'))

    def test_claude_context_is_not_cumulative(self):
        s={'context_window':{'total_input_tokens':1234567},'rate_limits':{'five_hour':{'used_percentage':45.5,'resets_at':1800000000}}}
        a=p.normalize_claude(s,None);self.assertIsNone(a['tokens']);self.assertEqual(a['windows'][0]['usedPercent'],45.5)

    def test_transcript_incremental_dedup_and_partial_line(self):
        with tempfile.TemporaryDirectory() as d:
            directory=Path(d);file=directory/'session.jsonl';db=p.database(directory)
            def record(mid,n):return json.dumps({'type':'assistant','message':{'id':mid,'content':'DO NOT RETAIN','usage':{'input_tokens':n,'output_tokens':10}}})+'\n'
            file.write_text(record('a',100)+record('a',100)+record('b',200),encoding='utf-8')
            self.assertEqual(p.transcript_tokens(db,str(file),'session'),320)
            self.assertEqual(p.transcript_tokens(db,str(file),'session'),320)
            with file.open('a') as f:f.write(record('a',150));f.write(record('c',300)[:-1])
            self.assertEqual(p.transcript_tokens(db,str(file),'session'),370)
            with file.open('a') as f:f.write('\n')
            self.assertEqual(p.transcript_tokens(db,str(file),'session'),680)
            # A compacted/rotated transcript does not subtract or replay tokens.
            file.write_text(record('a',100),encoding='utf-8')
            self.assertEqual(p.transcript_tokens(db,str(file),'session'),680)
            db.commit();db.close()
            self.assertNotIn(b'DO NOT RETAIN',(directory/'metrics.sqlite3').read_bytes())

    def test_snapshot_and_wire(self):
        with tempfile.TemporaryDirectory() as d:
            directory=Path(d)
            sample=p.publish(directory,{'provider':1,'tokens':12345,'windows':[{'usedPercent':92.5,'resetsAt':2000000000},None]})
            second=p.publish(directory,dict(sample,tokens=13000))
            self.assertEqual(second['stream'],sample['stream']);self.assertEqual(second['sequence'],sample['sequence']+1)
            wire=p.pack(sample);raw=bytes.fromhex(wire[2:-1].decode())
            self.assertEqual(len(wire),99);self.assertEqual(len(raw),48)
            self.assertEqual(zlib.crc32(raw[:44]),struct.unpack_from('<I',raw,44)[0])
            self.assertEqual(struct.unpack_from('<Q',raw,16)[0],12345)
            self.assertEqual(raw[3],3)
            with closing(p.database(directory)) as db:
                self.assertEqual(json.loads(db.execute('select value from snapshots where provider=1').fetchone()[0])['sequence'],second['sequence'])


if __name__=='__main__':unittest.main()
