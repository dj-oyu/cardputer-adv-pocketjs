"""Generation dependencies must remain guarded after the asset has been packed."""
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/kasane_contract'))
from run_fox_app_host import inputs

class Inputs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(['node','tools/ayatsuri/model/studies/fox_local_plane.cjs'],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)

    def test_source_mutation_is_detected_after_generation(self):
        before=inputs('mask-inset');read=Path.read_bytes
        for name in ('character.js','character_blend.js'):
            target=ROOT/'apps/character'/name;key=target.relative_to(ROOT).as_posix()
            with self.subTest(name=name):
                self.assertIn(key,before)
                def changed(path):
                    value=read(path)
                    return value+b'\n/* stale-input negative */' if path.resolve()==target.resolve() else value
                with patch.object(Path,'read_bytes',changed):after=inputs('mask-inset')
                self.assertNotEqual(before[key],after[key]);self.assertNotEqual(before,after)

if __name__=='__main__':unittest.main()
