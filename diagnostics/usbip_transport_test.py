"""Raw USB/IP regression; detach Windows first. No target access or writes.

Checks response wake during fragmented TCP headers/payloads, endpoint isolation,
cancellation, full windows and reconnection while old commands are outstanding.
"""
import argparse
import json
import socket
import struct
from pathlib import Path


def exact(s, size):
    result = bytearray()
    while len(result) < size:
        data = s.recv(size - len(result))
        if not data:
            raise EOFError('USB/IP connection closed')
        result.extend(data)
    return bytes(result)


class Client:
    def __init__(self, host):
        self.s = socket.create_connection((host, 3240), timeout=10)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.s.sendall(struct.pack('>HHI', 0x111, 0x8003, 0) + b'1-1'.ljust(32, b'\0'))
        assert exact(self.s, 8) == struct.pack('>HHI', 0x111, 3, 0)
        exact(self.s, 312)
        self.seq = 0
        self.directions = {}

    def frame(self, reading, data=b'', ep=1, length=512):
        self.seq += 1
        self.directions[self.seq] = reading
        header = struct.pack('>10I8s', 1, self.seq, 0x10001, int(reading), ep,
                             0, length if reading else len(data), 0, 0, 0, b'\0'*8)
        return self.seq, header + (b'' if reading else data)

    def submit(self, reading, data=b'', ep=1):
        seq, wire = self.frame(reading, data, ep)
        self.s.sendall(wire)
        return seq

    def receive(self):
        h = exact(self.s, 48)
        command, seq, devid, direction, ep, status, length = struct.unpack('>5Iii', h[:28])
        assert command in (3, 4) and (devid, direction, ep) == (0, 0, 0)
        data = exact(self.s, length) if command == 3 and self.directions.pop(seq) else b''
        return seq, status, length, data

    def info(self, info=4):
        out = self.submit(False, bytes([0, info]))
        assert self.receive() == (out, 0, 2, b'')
        inp = self.submit(True)
        seq, status, _, data = self.receive()
        assert (seq, status) == (inp, 0) and data[:1] == b'\0'
        return data

    def close(self):
        # Explicit shutdown: Winsock close alone can keep unsent data alive
        # in the background, leaving a test connection half-open.
        try:
            self.s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.s.close()


parser = argparse.ArgumentParser()
parser.add_argument('--host', default='192.168.4.1')
args = parser.parse_args()
report = {}
c = Client(args.host)
try:
    expected = c.info()
    capacity = int.from_bytes(c.info(0xff)[2:], 'little')
    assert 512 <= capacity <= 2048
    assert c.info(0xfe)[2] == 8

    for mode in ('header', 'payload'):
        # Delay command executes only in the probe; no SWD access.
        inp = c.submit(True)
        out = c.submit(False, b'\x09\x30\x75')  # 30 ms
        seq, wire = c.frame(False, b'\0\x04')
        split = 7 if mode == 'header' else 49
        c.s.sendall(wire[:split])
        responses = {r[0]: r for r in (c.receive(), c.receive())}
        assert responses[out] == (out, 0, 3, b'')
        assert responses[inp] == (inp, 0, 2, b'\x09\0')
        c.s.sendall(wire[split:])
        assert c.receive() == (seq, 0, 2, b'')
        inp = c.submit(True)
        assert c.receive() == (inp, 0, len(expected), expected)
        report[f'partial_{mode}_response_wake'] = 'PASS'

    out = c.submit(False, b'\0\x04')
    assert c.receive() == (out, 0, 2, b'')
    swo = c.submit(True, ep=2)
    assert c.receive() == (swo, 0, 0, b'')
    inp = c.submit(True)
    assert c.receive() == (inp, 0, len(expected), expected)
    for reading, ep in ((False, 2), (True, 0x81)):
        seq = c.submit(reading, b'\0\x04', ep)
        assert c.receive() == (seq, -32, 0, b'')  # EPIPE
    report['swo_isolated_and_invalid_endpoints_stall'] = 'PASS'

    for _ in range(20):
        outs = [c.submit(False, b'\0\x04') for _ in range(8)]
        for seq in outs:
            assert c.receive() == (seq, 0, 2, b'')
        ins = [c.submit(True) for _ in range(8)]
        for seq in ins:
            assert c.receive() == (seq, 0, len(expected), expected)
    report['eight_outstanding_requests'] = 'PASS: 20 windows'

    inp = c.submit(True)
    c.seq += 1
    c.s.sendall(struct.pack('>6I24s', 2, c.seq, 0x10001, 1, 1, inp, b'\0'*24))
    assert c.receive() == (c.seq, -104, 0, b'')
    assert c.info() == expected
    report['unlink_then_retry'] = 'PASS'
    print(json.dumps(report, indent=2), flush=True)
finally:
    c.close()

for _ in range(5):
    print(f'Reconnect {_ + 1}/5', flush=True)
    c = Client(args.host)
    c.submit(False, b'\x09\x30\x75')
    for i in range(7):
        c.submit(False, b'\0\x01')
    c.close()
    c = Client(args.host)
    try:
        assert c.info() == expected, 'Response leaked from detached session'
    finally:
        c.close()
report['detach_with_pending_commands'] = 'PASS: 5 reconnects'
Path('diagnostics/results/usbip-transport.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
