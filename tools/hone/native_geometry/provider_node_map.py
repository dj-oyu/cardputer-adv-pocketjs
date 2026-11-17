"""Bind partition ordinals to explicitly assigned model node IDs.

This emits private provider metadata only. It does not assign scene transforms,
construct model nodes, or admit the package for execution.
"""
import json
import pathlib
import sys
from provider_binding import binding_plan


def emit(package, assignments):
    parts = binding_plan(package)['nodes']
    if not isinstance(assignments, list) or len(assignments) > 256:
        raise ValueError('model node limit')
    if any(x is not None and (type(x) is not int or x < 0 or x >= len(parts)) for x in assignments):
        raise ValueError('invalid part ordinal')
    if sorted(x for x in assignments if x is not None) != list(range(len(parts))):
        raise ValueError('each part must be assigned exactly once')
    rows, arrays = [], []
    for node_id, ordinal in enumerate(assignments):
        if ordinal is None:
            rows.append('{UINT32_MAX,0,0}')
            continue
        part = parts[ordinal]
        vertices = part['prepared_vertices']
        if not 1 <= len(vertices) <= 32:
            raise ValueError('part vertex count')
        name = 'hone_provider_node_%d_vertices' % node_id
        arrays.append('static const uint32_t %s[] = {%s};\n' % (name, ','.join(map(str, vertices))))
        rows.append('{%d,%d,%s}' % (part['source_mesh'], len(vertices), name))
    header = ['/* Private generated provider bindings, not runtime admission. */\n',
              '#ifndef HONE_PRIVATE_PROVIDER_MAP_H\n#define HONE_PRIVATE_PROVIDER_MAP_H\n',
              '/* Include native_provider.h first. */\n']
    header.extend(arrays)
    header.append('static const hone_provider_part hone_provider_nodes[] = {%s};\n' % (','.join(rows) if rows else '{UINT32_MAX,0,0}'))
    header.append('static const size_t hone_provider_node_count = %d;\n#endif\n' % len(rows))
    return ''.join(header)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit('expected private source package, node assignment JSON, new output header')
    package = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf8'))
    assignments = json.loads(pathlib.Path(sys.argv[2]).read_text(encoding='utf8'))
    content = emit(package, assignments)
    with pathlib.Path(sys.argv[3]).open('x', encoding='utf8', newline='\n') as output:
        output.write(content)
