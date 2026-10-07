"""Compare equal DP read workloads using 512B-sized vs 1024B-sized commands.

Reads DPIDR only; no target program or SRAM writes. This measures protocol
batching potential, not Keil Flash programming throughput.
"""
import json
from pathlib import Path
import statistics
import time
from dap_probe import Probe


def summary(values):
    ordered = sorted(values)
    return {'median_ms': round(statistics.median(values), 3),
            'p95_ms': round(ordered[int(len(ordered) * .95)], 3)}


p = Probe('bulk', pid=0x4043)
try:
    p.packet_size = int.from_bytes(p.command([0, 0xff])[2:], 'little')
    assert p.packet_size >= 1024
    dpidr = p.connect()
    timings = {'two_127_word_commands': [], 'one_254_word_command': []}
    for trial in range(20):
        # Alternate ordering to reduce short-term Wi-Fi drift bias.
        cases = [(127, 2), (254, 1)]
        if trial % 2:
            cases.reverse()
        for words, commands in cases:
            start = time.perf_counter()
            for _ in range(commands):
                assert p.block(2, words) == [dpidr] * words
            elapsed = (time.perf_counter() - start) * 1000
            key = 'two_127_word_commands' if commands == 2 else 'one_254_word_command'
            timings[key].append(elapsed)
    report = {'note': 'Equal 1016-byte DPIDR read workload, not Flash download speed',
              'rounds_per_case': 20, 'packet_capacity': p.packet_size,
              **{key: summary(value) for key, value in timings.items()}}
    p.command([3])
    Path('diagnostics/results/large-packet-benchmark.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)
finally:
    p.close()
