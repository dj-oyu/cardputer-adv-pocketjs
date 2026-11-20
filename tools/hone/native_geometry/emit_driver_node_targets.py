"""Emit an explicitly partial driver-to-node table; other outputs stay pending."""
import collections
import json
import pathlib
import sys


def emit(plan):
    if plan.get('schema')!='hone.driver-route-plan.v1' or plan.get('runtime_ready') is not False:
        raise ValueError('unsupported route plan')
    rows=[];remaining=collections.Counter();seen=set()
    channels={'location':0,'rotation_euler':3,'scale':6}
    for route in plan['routes']:
        kind=route['kind'];program=route['program']
        if type(program) is not int or not 0<=program<=65535:raise ValueError('program index')
        if kind=='mesh_visibility':channel=9
        elif kind=='parent_transform':
            if route.get('direct_xyz_property_pose') is not True:raise ValueError('parent pose not admitted')
            if route['data_path'] not in channels or route['array_index'] not in (0,1,2):raise ValueError('pose channel')
            channel=channels[route['data_path']]+route['array_index']
        else:
            remaining[kind]+=1
            continue
        nodes=route.get('node_ids')
        if not isinstance(nodes,list) or not nodes:raise ValueError('missing node targets')
        for node in nodes:
            if type(node) is not int or not 0<=node<256 or (node,channel) in seen:raise ValueError('duplicate or invalid node target')
            seen.add((node,channel));rows.append((program,node,channel))
    if len(rows)>4096:raise ValueError('route limit')
    body=','.join('{%d,%d,%d}'%row for row in rows) if rows else '{0,0,0}'
    header=('/* Generated candidate: incomplete driver routing, not runtime-ready. */\n'
            '#ifndef HONE_PRIVATE_DRIVER_NODES_H\n#define HONE_PRIVATE_DRIVER_NODES_H\n'
            '#include "driver_node_targets_candidate.h"\n'
            'static const hone_driver_node_route hone_driver_node_routes[] = {%s};\n'
            'static const size_t hone_driver_node_route_count = %d;\n#endif\n')%(body,len(rows))
    return header,dict(runtime_ready=False,node_routes=len(rows),unbound_programs=sum(remaining.values()),unbound_kinds=dict(remaining))


if __name__=='__main__':
    if len(sys.argv)!=3:raise SystemExit('expected driver route plan and new header')
    plan=json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf8'))
    header,summary=emit(plan)
    with pathlib.Path(sys.argv[2]).open('x',encoding='utf8',newline='\n') as out:out.write(header)
    print(json.dumps(summary))
