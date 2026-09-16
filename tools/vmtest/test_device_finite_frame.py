import unittest

from device_finite_frame import outcome


class FiniteFrameTest(unittest.TestCase):
    def test_completed(self):
        self.assertEqual(outcome("VM_FINITE_DONE n=20000 sum=199990000", 20000),
                         {"n": 20000, "completed": True})

    def test_guarded(self):
        self.assertEqual(outcome("RUNAWAY one frame spent 257000 us\nAPP_STOPPED", 100000),
                         {"n": 100000, "completed": False, "execution_us": 257000})

    def test_invalid_results(self):
        for text in ("APP_STOPPED", "VM_FINITE_DONE n=20000 sum=0",
                     "VM_FINITE_DONE n=40000 sum=799980000",
                     "RUNAWAY one drain spent 257000 us\nAPP_STOPPED",
                     "VM_FINITE_DONE n=20000 sum=199990000\nRUNAWAY one frame spent 257000 us"):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                outcome(text, 20000)


if __name__ == "__main__":
    unittest.main()
