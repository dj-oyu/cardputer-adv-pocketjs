#!/usr/bin/env python3
"""Keep audit selection independent of filenames and directory conventions."""
import unittest

from async_audit import ASYNC, DEPTH


class SelectionTest(unittest.TestCase):
    def test_async_forms(self):
        for source in ["class C { async #f() {} }", "({ async *f() {} })",
                       "await 0", "features: [asyncFunctions]",
                       "features: [asyncIteration]", "flags: [async]"]:
            with self.subTest(source=source):
                self.assertIsNotNone(ASYNC.search(source))

    def test_depth_without_async(self):
        for source in ["Recursive invocation", "stack overflow",
                       "maximum call stack", "stack depth", "stack size limit"]:
            with self.subTest(source=source):
                self.assertIsNotNone(DEPTH.search(source))

    def test_unrelated(self):
        self.assertIsNone(ASYNC.search("var asynchronous = 0"))
        self.assertIsNone(DEPTH.search("stack.push(value)"))


if __name__ == "__main__":
    unittest.main()
