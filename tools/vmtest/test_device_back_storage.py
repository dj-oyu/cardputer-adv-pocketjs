"""Exercise the serial protocol without a board; these are not device results."""
import unittest

from device_back_storage import run


class Port:
    def __init__(self, lines):
        self.lines = iter(lines)
        self.commands = []

    def write(self, data):
        self.commands.append(data)

    def readline(self):
        return (next(self.lines) + "\n").encode()


class ProtocolTest(unittest.TestCase):
    def test_write_order(self):
        port = Port(["HOME_READY", "VM_SAVE_MARK 1", "VM_SAVE_MARK 10", "VM_SAVE_MARK 11", "VM_SAVE_MARK 2",
                     "VM_SAVE_MARK 3", "VM_SAVE_MARK 4", "VM_SAVE_MARK 5", "HOME_READY"])
        run(port, "write")
        self.assertEqual(port.commands, [b"q", b"Y", b"q"])

    def test_read_cleanup(self):
        port = Port(["HOME_READY", "VM_SAVE_MARK 6", "VM_SAVE_MARK 7",
                     "VM_SAVE_MARK 8", "HOME_READY"])
        run(port, "read")
        self.assertEqual(port.commands, [b"q", b"Z", b"q"])

    def test_stop_before_save_fails(self):
        port = Port(["HOME_READY", "VM_SAVE_MARK 1", "VM_SAVE_MARK 10", "VM_SAVE_MARK 11", "VM_SAVE_MARK 2",
                     "VM_SAVE_MARK 4", "VM_SAVE_MARK 3", "VM_SAVE_MARK 5", "HOME_READY"])
        with self.assertRaisesRegex(RuntimeError, "Unexpected save/stop order"):
            run(port, "write")

    def test_existing_record_refusal(self):
        port = Port(["HOME_READY", "VM_SAVE_MARK 9"])
        with self.assertRaisesRegex(RuntimeError, "rejected"):
            run(port, "write")
        self.assertEqual(port.commands, [b"q", b"Y"])

    def test_back_without_suspension_fails(self):
        port = Port(["HOME_READY", "VM_SAVE_MARK 1", "VM_SAVE_MARK 10", "VM_SAVE_MARK 9"])
        with self.assertRaisesRegex(RuntimeError, "rejected"):
            run(port, "write")

    def test_missing_suspension_check_fails(self):
        port = Port(["HOME_READY", "VM_SAVE_MARK 1", "VM_SAVE_MARK 10", "VM_SAVE_MARK 2",
                     "VM_SAVE_MARK 3", "VM_SAVE_MARK 4", "VM_SAVE_MARK 5", "HOME_READY"])
        with self.assertRaisesRegex(RuntimeError, "Unexpected save/stop order"):
            run(port, "write")

    def test_missing_completion_fails(self):
        port = Port(["HOME_READY", "VM_SAVE_MARK 1", "VM_SAVE_MARK 10", "VM_SAVE_MARK 11", "VM_SAVE_MARK 2", "HOME_READY"])
        with self.assertRaisesRegex(RuntimeError, "Unexpected save/stop order"):
            run(port, "write")


if __name__ == "__main__":
    unittest.main()
