"""Emit ordered admitted wrap identities, never evaluated target data."""
import json,math,pathlib,sys


def emit(plan):
    if plan.get('schema')!='hone.shrinkwrap-route-plan.v1' or plan.get('runtime_ready') is not False:
        raise ValueError('unsupported wrap plan')
    rows=[];seen=set();sources={r['source_mesh'] for r in plan['routes']}
    for r in plan['routes']:
        src=r['source_mesh'];target=r['target_mesh'];offset=r['offset']
        if (type(src) is not int or type(target) is not int or src<0 or target<0 or src>0xffffffff or target>0xffffffff or
            src==target or src in seen or (target in sources and target not in seen) or
            type(offset) not in (int,float) or not math.isfinite(offset)):
            raise ValueError('invalid wrap route')
        seen.add(src);rows.append('{%d,%d,%s}'%(src,target,float(offset).hex()))
    h=('/* Generated wrap identities; transforms and target pose are live inputs. */\n'
       '#ifndef HONE_PRIVATE_WRAP_TARGETS_H\n#define HONE_PRIVATE_WRAP_TARGETS_H\n'
       '#include <stdint.h>\n#include <stddef.h>\n'
       'typedef struct {uint32_t source_mesh,target_mesh;double offset;} hone_wrap_route;\n'
       'static const hone_wrap_route hone_wrap_routes[] = {%s};\n'
       'static const size_t hone_wrap_route_count = %d;\n#endif\n')%(','.join(rows) if rows else '{0,0,0}',len(rows))
    return h,dict(runtime_ready=False,wrap_routes=len(rows))


if __name__=='__main__':
    if len(sys.argv)!=3:raise SystemExit('expected wrap route plan and new header')
    plan=json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf8'));header,summary=emit(plan)
    with pathlib.Path(sys.argv[2]).open('x',encoding='utf8',newline='\n') as out:out.write(header)
    print(json.dumps(summary))
