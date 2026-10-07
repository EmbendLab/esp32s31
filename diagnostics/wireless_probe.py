"""Read wireless DAP identity over the Windows WinUSB API; no target writes."""
import json
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import time
from dap_probe import GUID, Probe, paths

parser = argparse.ArgumentParser()
parser.add_argument('--target', action='store_true', help='Also read target DPIDR and CPUID')
args = parser.parse_args()
guid = GUID.parse('cdb3b5ad-293b-4663-aa36-1aae46463776')
print('DAP interfaces:', list(paths(guid)), flush=True)
p = Probe('bulk', pid=0x4043)
report = {'pid': '4043', 'backend': 'WinUSB', 'info': {}}
try:
    expected = {}
    for info in (1, 2, 3, 4, 9, 0xF0, 0xFE, 0xFF):
        sent = p.io(False, bytes([0, info]))
        print(f'Info {info:02x}: OUT actual_length={sent}', flush=True)
        assert sent == 2, sent
        response = p.io(True, timeout=3000)
        assert len(response) >= 2 and response[0] == 0, response.hex()
        assert len(response) == response[1] + 2, response.hex()
        expected[info] = response
        report['info'][f'{info:02x}'] = response.hex()
        print(json.dumps({'info': info, 'length': len(response), 'hex': response.hex()}), flush=True)
    window = expected[0xFE][2]
    p.packet_size = int.from_bytes(expected[0xFF][2:], 'little')
    assert 512 <= p.packet_size <= 2048
    report['packet_capacity'] = p.packet_size
    assert p.command(bytes([0, 4]).ljust(p.packet_size, b'\0')) == expected[4]
    report['full_size_out'] = 'PASS'
    for _ in range(20):
        ids = list(expected)[:window]
        for info in ids:
            p.send([0, info])
        for info in ids:
            assert p.receive() == expected[info]
    report['queued_windows'] = 'PASS: 20 windows'
    with ThreadPoolExecutor(max_workers=1) as pool:
        read = pool.submit(p.io, True, timeout=3000)
        time.sleep(0.1)
        assert not read.done(), 'Idle IN completed without DAP data'
        p.send([0, 4])
        assert read.result() == expected[4]
    report['in_before_out'] = 'PASS'
    try:
        p.io(True, timeout=150)
        raise AssertionError('Idle IN unexpectedly completed')
    except TimeoutError:
        pass
    assert p.command([0, 4]) == expected[4]
    report['unlink_then_retry'] = 'PASS'
    if args.target:
        report['dpidr'] = hex(p.connect())
        count = (p.packet_size - 4) // 4
        assert p.block(2, count) == [int(report['dpidr'], 16)] * count
        report['full_size_block_read'] = f'PASS: {count} words, {4 + 4*count} response bytes'
        # Match-mask updates exercise a large Transfer request without target writes.
        count = min(255, (p.packet_size - 3) // 5)
        p.xfer([(0x20, 0xffffffff)] * count)
        report['large_transfer_out'] = f'PASS: {3 + 5*count} bytes, no target writes'
        # One extra read word must fail rather than overrun the response buffer.
        import struct
        response = p.command(b'\x06\0' + struct.pack('<HB', (p.packet_size-4)//4 + 1, 2))
        assert response == b'\x06\0\0\x08', response.hex()
        report['oversize_block_rejected'] = 'PASS'
        report['cpuid'] = hex(p.read(0xE000ED00)[0])
        p.command([3])
    Path('diagnostics/results/wireless-winusb.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)
finally:
    p.close()
