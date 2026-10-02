"""Enumerate a COM identity immediately before use; never opens/resets a port.

This narrows re-enumeration races, but is not an OS-level exclusive claim or
cryptographic device identity. Existing device locks cover this harness only.
"""
import argparse
import hashlib
import json
import re


def list_port_records():
    from serial.tools import list_ports
    return [{'device': p.device.upper(), 'description': p.description or '', 'hwid': p.hwid or '',
             'vid': p.vid, 'pid': p.pid, 'serial_number': p.serial_number, 'location': p.location}
            for p in list_ports.comports() if re.fullmatch(r'COM[1-9][0-9]*', p.device.upper())]


def signature(port):
    return hashlib.sha256(json.dumps(port, sort_keys=True).encode()).hexdigest()


def verify(port, expected, rows=None):
    rows = list_port_records() if rows is None else rows
    matches = [row for row in rows if row['device'] == port]
    if len(matches) != 1 or signature(matches[0]) != expected:
        raise ValueError('COM identity changed or disappeared; prepare a new GUI plan.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--signature', required=True)
    args = parser.parse_args()
    verify(args.port, args.signature)
    print('Enumerated COM identity matches the approved plan; no port was opened.')
