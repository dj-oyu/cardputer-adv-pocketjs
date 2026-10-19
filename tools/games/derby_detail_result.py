"""Strict detailed stream paired with the existing whole-trial quality gate."""
import json
from pathlib import Path
import re
from derby_fixed_result import parse_trial
SCHEMA=json.loads((Path(__file__).with_name('derby_detail_schema.json')).read_text())

def parse_detail(text,*,expected):
    trial=parse_trial(text.encode('ascii'),transport_complete=True)
    all_lines=text.splitlines()
    # A valid trailer cannot qualify a run that faults afterwards, before HOME.
    plain=re.sub(r'\x1b\[[0-9;]*m','',text)
    if re.search(r'InternalError:|FRAME RUNAWAY|RUNAWAY|LOADSTALL|Guru Meditation|assert failed|stack overflow|watchdog|\bOOM\b|abort\(\)|hook (?:rejected|threw):|stop hook did not finish',plain,re.I):
        raise ValueError('detail runtime fault in complete transport')
    # ESP-IDF device markers have an info prefix; host fixtures use bare ones.
    # Restrict tag and entire payload so unrelated text cannot prove teardown.
    def marker(line,tag,payload):
        return re.fullmatch(r'(?:I \([0-9]+\) '+tag+r': )?'+payload,
                            re.sub(r'\x1b\[[0-9;]*m','',line)) is not None
    stop=[i for i,l in enumerate(all_lines) if marker(l,'app','APP_STOPPED')]
    home=[i for i,l in enumerate(all_lines) if marker(l,'shell','HOME_READY')]
    fixed_end_lines=[i for i,l in enumerate(all_lines) if l.startswith('DERBY_FIXED_END ')]
    if len(stop)!=1 or len(home)!=1 or len(fixed_end_lines)!=1 or not fixed_end_lines[0]<stop[0]<home[0]:
        raise ValueError('detail STOP/HOME completion')
    lines=[line for line in text.splitlines() if line.startswith(('DERBY_DETAIL','DERBY_FIXED'))]
    if not expected:
        if any(line.startswith('DERBY_DETAIL') for line in lines):raise ValueError('detail present in OFF')
        return []
    result=[];position=0
    def take(marker,keys):
        nonlocal position
        if position>=len(lines):raise ValueError('missing detailed record')
        parts=lines[position].split();position+=1
        if parts[0]!=marker:raise ValueError('detailed order/marker')
        pairs=[p.split('=') for p in parts[1:]]
        if any(len(p)!=2 for p in pairs) or len({p[0] for p in pairs})!=len(pairs):raise ValueError('duplicate/malformed detail field')
        row=dict(pairs)
        if set(row)!=set(keys.split()):raise ValueError('detail schema drift')
        if any(not re.fullmatch(r'0|[1-9][0-9]*',v) or int(v)>2**64-1 for v in row.values()):raise ValueError('detail unsigned integer')
        return {k:int(v) for k,v in row.items()}
    for rep,summary in enumerate(trial['repetitions'],1):
        identity={'v':1,'session':int(trial['session']),'rep':rep}
        def check_identity(row):
            if any(row[k]!=v for k,v in identity.items()):raise ValueError('detail identity')
        stages=[];events=[]
        for i in range(len(SCHEMA['stages'])):
            row=take('DERBY_DETAIL','v session rep stage inc_us self_us calls');check_identity(row)
            if row['stage']!=i or row['self_us']>row['inc_us'] or row['inc_us']>int(summary['work_us']):raise ValueError('detail stage or impossible time')
            if not row['calls'] and row['inc_us']:raise ValueError('time without calls')
            stages.append(row)
        for i in range(len(SCHEMA['events'])):
            row=take('DERBY_DETAIL_BYTES','v session rep event amount');check_identity(row)
            if row['event']!=i:raise ValueError('detail event sequence')
            events.append(row['amount'])
        end=take('DERBY_DETAIL_END','v session rep stages events faults pairs elapsed logical_bytes');check_identity(end)
        if end['stages']!=len(stages) or end['events']!=len(events) or end['faults'] or end['elapsed']!=1 or end['logical_bytes']!=1:raise ValueError('detail totals/quality')
        if end['pairs']!=sum(r['calls'] for r in stages) or sum(r['self_us'] for r in stages)>int(summary['work_us']):raise ValueError('detail accounting exceeds work')
        if position>=len(lines) or not lines[position].startswith('DERBY_FIXED v='):raise ValueError('detail summary pair')
        position+=1
        if [r['calls'] for r in stages[:7]]!=[120,120,240,120,120,120,720]:
            raise ValueError('detail fixed JS/native work missing')
        builds=int(summary['s3'].split(':')[1])
        if stages[7]['calls']!=builds or stages[14]['calls']!=builds or any(not stages[i]['calls'] for i in range(8,22)):
            raise ValueError('detail procedural/render work missing')
        if events[0]!=(38400 if trial['variant']=='batch' else 0) or any(not events[i] for i in range(1,8)):
            raise ValueError('detail logical byte work missing')
        if events[SCHEMA['events'].index('TX_FAILURES')]:raise ValueError('detail TX failure')
        result.append({'rep':rep,'stages':stages,'events':dict(zip(SCHEMA['events'],events,strict=True)),'pairs':end['pairs']})
    if position>=len(lines) or not lines[position].startswith('DERBY_FIXED_END '):raise ValueError('extra/missing detail records')
    position+=1
    exit_row=take('DERBY_DETAIL_EXIT','v session clean')
    if exit_row!={'v':1,'session':int(trial['session']),'clean':1} or position!=len(lines):
        raise ValueError('detail owner exit record')
    exit_lines=[i for i,l in enumerate(all_lines) if l.startswith('DERBY_DETAIL_EXIT ')]
    if len(exit_lines)!=1 or not fixed_end_lines[0]<exit_lines[0]<stop[0]<home[0]:
        raise ValueError('detail owner exit order')
    for row in result[1:]:
        if [x['calls'] for x in row['stages'][:15]]!=[x['calls'] for x in result[0]['stages'][:15]] or list(row['events'].values())[:4]!=list(result[0]['events'].values())[:4]:
            raise ValueError('dependent replay detail work changed')
    return result
