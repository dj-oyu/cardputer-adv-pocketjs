"""Execute retained draw wrappers and check their capture/native-call contract."""
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from check_equivalence import instrument, instrument_draw

ROOT = Path(__file__).resolve().parents[2]
CURRENT = "const dr = (n, a, b, c, d, e, f, g, h) => { if (live[n]) H.draw(live[n], a, b, c, d, e, f, g, h); };"
LEGACY = "const dr = (n, a) => { if (live[n]) H.draw(live[n], a); };"


def execute(wrapper, calls):
    script = r"""
const calls = [], logs = [], errors = [], live = {p: 17};
const H = {draw(...args) {
  calls.push(args.map(v => v === undefined ? {undefined: true} : v));
  let values = args.slice(1);
  while (values.length > 1 && values.at(-1) === undefined) values.pop();
  if (Array.isArray(values[0])) {
    if (values.length !== 1) throw Error('extra array argument');
    values = values[0];
  }
  if (values.length > 8 || values.some(v => typeof v !== 'number' || !Number.isFinite(v)))
    throw Error('invalid input');
}};
const __log = value => logs.push(value);
WRAPPER
for (const call of CALLS) { try { call(); } catch (e) { errors.push(e.message); } }
console.log(JSON.stringify({calls, logs, errors}));
""".replace("WRAPPER", wrapper).replace("CALLS", "[" + ",".join("()=>" + c for c in calls) + "]")
    return json.loads(subprocess.check_output(["node", "-e", script], text=True))


class CaptureTests(unittest.TestCase):
    def compare(self, wrapper, calls, expected):
        plain = execute(wrapper, calls)
        captured = execute(instrument_draw(wrapper), calls)
        self.assertEqual(captured["calls"], plain["calls"], "native argument count/value/order changed")
        self.assertEqual(captured["errors"], plain["errors"], "native rejection changed")
        self.assertEqual(captured["logs"], expected)

    def test_legacy_array_and_missing_handle(self):
        self.compare(LEGACY, ["dr('p',[1,0,-2.5])", "dr('p',[])", "dr('missing',[9])"],
                     ["DRAW p 1,0,-2.5", "DRAW p "])

    def test_current_scalar_array_and_trailing_undefined(self):
        self.compare(CURRENT, ["dr('p',1,0,-2.5)", "dr('p',0,undefined,undefined)",
                               "dr('p',1,2,3,4,5,6,7,8)", "dr('p',[4,0,6])", "dr('p',[])",
                               "dr('missing',1,2)"],
                     ["DRAW p 1,0,-2.5", "DRAW p 0", "DRAW p 1,2,3,4,5,6,7,8",
                      "DRAW p 4,0,6", "DRAW p "])

    def test_native_rejections_are_not_captured_as_valid_draws(self):
        self.compare(CURRENT, ["dr('p',1,undefined,3)", "dr('p',Infinity)",
                               "dr('p',[1,undefined,3])", "dr('p',[1,2],3)"], [])

    def test_array_capture_ignores_guest_copy_methods(self):
        self.compare(CURRENT, ["dr('p',Object.assign([1,2],{slice(){throw Error('guest slice')},"
                               "join(){throw Error('guest join')}}))"], ["DRAW p 1,2"])

    def test_accessor_array_fails_capture_without_a_second_guest_read(self):
        script = r"""
let native = 0, reads = 0, error = '', logs = [];
const live = {p: 17}, __log = s => logs.push(s);
const H = {draw(handle, a) { native++; if (a[0] !== 3) throw Error('native input'); }};
WRAPPER
const input = [0]; Object.defineProperty(input, 0, {get() { reads++; return 3; }});
try { dr('p', input); } catch (e) { error = e.message; }
console.log(JSON.stringify({native, reads, error, logs}));
""".replace("WRAPPER", instrument_draw(CURRENT))
        result = json.loads(subprocess.check_output(["node", "-e", script], text=True))
        self.assertEqual(result, {"native": 1, "reads": 1,
                                 "error": "DRAW capture requires data-array inputs", "logs": []})

    def test_unknown_duplicate_and_changed_forwarding_fail_closed(self):
        for source in ("", CURRENT + "\n" + CURRENT, LEGACY + "\n" + CURRENT,
                       CURRENT.replace("H.draw(live[n], a, b", "H.draw(live[n], b, a")):
            with self.subTest(source=source), self.assertRaisesRegex(AssertionError, "exactly one known"):
                instrument_draw(source)

    def test_current_source_registration_and_wrapper_capture(self):
        source = (ROOT / "apps/derby/derby_view.js").read_text(encoding="utf-8")
        self.assertEqual(source.count(CURRENT), 1, "update the capture contract for a new wrapper")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); app = root / "apps/derby"; app.mkdir(parents=True)
            (app / "derby_view.js").write_text(source, encoding="utf-8")
            (app / "derby_prog.js").write_text("untouched ROM source", encoding="utf-8")
            instrument(root)
            changed = (app / "derby_view.js").read_text(encoding="utf-8")
            self.assertIn("__log('REG ' + n + ' ' + JSON.stringify(p[1] || []));", changed)
            wrapper = re.search(r"^const dr = .*;$", changed, flags=re.M)[0]
            self.assertEqual(execute(wrapper, ["dr('p',2,0,3)"])["logs"], ["DRAW p 2,0,3"])
            self.assertEqual((app / "derby_prog.js").read_text(), "untouched ROM source")


if __name__ == "__main__":
    unittest.main()
