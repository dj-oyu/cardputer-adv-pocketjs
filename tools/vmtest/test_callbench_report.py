import unittest
from callbench_report import EXPECTED, report


def fixture():
    lines = ["CALLBENCH PATH mode=flat span=0 checks=17", "CALLBENCH PATH mode=recur span=4608 checks=17"]
    for name, value in EXPECTED.items():
        for r in range(8):
            for s in range(4):
                mode = "recur" if ((s in (1, 2)) ^ bool(r & 1)) else "flat"
                ns = 100 if mode == "flat" else 200
                lines.append(f"CALLBENCH SAMPLE case={name} round={r} slot={s} mode={mode} ns={ns} value={value}")
    return "\n".join(lines + ["CALLBENCH PASS"])


class ReportTest(unittest.TestCase):
    def test_valid(self):
        result = report(fixture())
        self.assertEqual(result["samples"], 128)
        self.assertEqual(result["cases"]["call"]["paired_flat_over_recur"]["median"], 0.5)

    def test_invalid(self):
        text = fixture()
        for bad in (text.replace("CALLBENCH PASS", ""), text + "\nCALLBENCH PASS",
                    text.replace("span=4608", "span=0"), text.replace("checks=17", "checks=16"),
                    text.replace("value=17000", "value=0"), text.replace("ns=100", "ns=0"),
                    text.replace("slot=0", "slot=1"), text.replace("round=7", "round=8"),
                    "\n".join(text.splitlines()[1:]), "\n".join(text.splitlines()[:-2])):
            with self.subTest(bad=bad[:80]), self.assertRaises(ValueError):
                report(bad)

    def test_inputs(self):
        text = "CALLBENCH KIND inputs\n" + fixture().replace("mode=flat", "mode=lazy").replace("mode=recur", "mode=eager").replace("span=4608", "span=0")
        self.assertEqual(report(text)["cases"]["call"]["paired_lazy_over_eager"]["median"], 0.5)
        for bad in (text.replace("KIND inputs", "KIND dispatch"), text + "\nCALLBENCH KIND inputs",
                    text.replace("mode=eager span=0", "mode=eager span=4608")):
            with self.assertRaises(ValueError):
                report(bad)


if __name__ == "__main__":
    unittest.main()
