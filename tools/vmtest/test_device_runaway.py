import unittest

from device_runaway import measurement


class MeasurementTest(unittest.TestCase):
    def test_frame(self):
        result = measurement("E app: RUNAWAY one frame spent 257016 us", "3")
        self.assertEqual(result, {"case": "3", "kind": "frame", "execution_us": 257016})

    def test_drain(self):
        result = measurement("RUNAWAY one drain spent 257301 us over 357 jobs in 30 turns", "6")
        self.assertEqual(result["execution_us"], 257301)
        self.assertEqual(result["kind"], "drain")

    def test_wrong_origin(self):
        with self.assertRaises(RuntimeError):
            measurement("RUNAWAY one drain spent 250000 us", "3")

    def test_missing(self):
        with self.assertRaises(RuntimeError):
            measurement("APP_STOPPED", "3")

    def test_duplicate(self):
        with self.assertRaises(RuntimeError):
            measurement("RUNAWAY one frame spent 250000 us\n" * 2, "3")


if __name__ == "__main__":
    unittest.main()
