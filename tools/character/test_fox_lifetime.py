"""Reject retained growth even when all six lifecycle samples are present."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'kasane_contract'))
from run_fox_app_host import lifetime_stable


class Lifetime(unittest.TestCase):
    def setUp(self):
        self.rows=[dict(name=name,rounded_request_current=100,guest_accounted_current=180)
                   for name in ('released','recreated') for _ in range(6)]

    def test_stable(self):
        self.assertTrue(lifetime_stable(self.rows))

    def test_growth_with_complete_sample_count(self):
        for name in ('released','recreated'):
            for key in ('rounded_request_current','guest_accounted_current'):
                with self.subTest(name=name,key=key):
                    rows=copy.deepcopy(self.rows)
                    next(row for row in rows if row['name']==name)[key]+=4
                    self.assertFalse(lifetime_stable(rows))

    def test_missing_sample(self):
        self.assertFalse(lifetime_stable(self.rows[:-1]))


if __name__=='__main__':unittest.main()
