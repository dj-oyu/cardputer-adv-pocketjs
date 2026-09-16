import unittest

from device_finite_frame import outcome


class FiniteFrameTest(unittest.TestCase):
    def test_completed(self):
        self.assertEqual(outcome("VM_FINITE_DONE n=20000 sum=199990000", 20000),
                         {"n": 20000, "completed": True})

    def test_guarded(self):
        self.assertEqual(outcome("RUNAWAY one frame spent 257000 us\nAPP_STOPPED", 100000),
                         {"n": 100000, "completed": False, "execution_us": 257000})

    def test_completed_timing(self):
        text = ("VM_FINITE_DONE n=20000 sum=199990000\n"
                "VM_FRAME_COMPLETE execution_us=120001 exception=0")
        self.assertEqual(outcome(text, 20000, require_timing=True),
                         {"n": 20000, "completed": True, "execution_us": 120001})

    def test_invalid_timing(self):
        done = "VM_FINITE_DONE n=20000 sum=199990000"
        marker = "VM_FRAME_COMPLETE execution_us=120001 exception=0"
        for text in (done, done + "\n" + marker.replace("exception=0", "exception=1"),
                     marker + "\n" + done, done + "\n" + marker + "\n" + marker,
                     "RUNAWAY one frame spent 257000 us\nAPP_STOPPED\n" + marker):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                outcome(text, 20000, require_timing=True)

    def test_invalid_results(self):
        for text in ("APP_STOPPED", "VM_FINITE_DONE n=20000 sum=0",
                     "VM_FINITE_DONE n=40000 sum=799980000",
                     "RUNAWAY one drain spent 257000 us\nAPP_STOPPED",
                     "VM_FINITE_DONE n=20000 sum=199990000\nRUNAWAY one frame spent 257000 us"):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                outcome(text, 20000)


if __name__ == "__main__":
    unittest.main()
