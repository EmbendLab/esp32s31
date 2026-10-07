"""Measure DAP_Info round trips only: no target connect, halt, or memory writes."""
import json
import argparse
from pathlib import Path
import statistics
import time
from dap_probe import Probe


def stats(values):
    ordered = sorted(values)
    return {
        'count': len(values),
        'min_ms': round(min(values), 3),
        'median_ms': round(statistics.median(values), 3),
        'p95_ms': round(ordered[min(len(values)-1, int(len(values)*0.95))], 3),
        'max_ms': round(max(values), 3),
    }


parser = argparse.ArgumentParser()
parser.add_argument('--output', default='diagnostics/results/wireless-latency.json')
args = parser.parse_args()
p = Probe('bulk', pid=0x4043)
try:
    size = int.from_bytes(p.command([0, 0xff])[2:], 'little')
    p.packet_size = size
    window = p.command([0, 0xfe])[2]
    expected = p.command([0, 4])
    report = {'packet_capacity_bytes': size, 'packet_window': window,
              'note': 'Probe information queries, not Flash download throughput'}
    for length in (2, size):
        packet = bytes([0, 4]).ljust(length, b'\0')
        times = []
        for _ in range(50):
            start = time.perf_counter()
            assert p.command(packet) == expected
            times.append((time.perf_counter()-start)*1000)
        report[f'round_trip_out_{length}_bytes'] = stats(times)
    times = []
    for _ in range(20):
        start = time.perf_counter()
        for i in range(window):
            p.send([0, 4])
        for i in range(window):
            assert p.receive() == expected
        times.append((time.perf_counter()-start)*1000)
    report['window_total_time'] = stats(times)
    report['window_commands_per_second'] = round(window*len(times)*1000/sum(times), 1)
    Path(args.output).write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)
finally:
    p.close()
