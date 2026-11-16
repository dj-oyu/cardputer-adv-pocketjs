"""Compare bounded replay against identical full-screen traversal, host only."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='alpha-bounds-') as t:
    for flags in [['-O2'],['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover']]:
        binary=Path(t)/'bounds'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-ffp-contract=off',*flags,
            '-I',str(root/'main'),'-I',str(root/'tools/hostshim'),
            str(root/'tools/kasane_contract/test_alpha_bounds.c'),
            *[str(root/x) for x in ['main/ui/kasane/ksn_mesh_depth.c','main/ui/kasane/ksn_shiki_frame.c','main/ayatsuri/compact_opaque.c']],
            '-lm','-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
