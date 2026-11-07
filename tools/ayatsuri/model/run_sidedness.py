"""Run sidedness contracts through the current Shiki frame integration.
Historical a426 paired captures belong to the preserved 4a4d handoff, not the
removed procedural VM. This entry runs current native synthetic contracts only.
"""
from pathlib import Path
import argparse, subprocess, sys
ROOT=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--opt',choices=['0','2'],default='2');a=p.parse_args()
subprocess.run([sys.executable,str(ROOT/'tools/kasane_contract/run_mesh_depth.py'),'--opt',a.opt],cwd=ROOT,check=True)
