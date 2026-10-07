"""Capture UART without toggling DTR/RTS or resetting the probe."""
import argparse
from pathlib import Path
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--seconds', type=float, default=15)
parser.add_argument('--output', default='diagnostics/results/usbip-uart.log')
parser.add_argument('--wait-port', type=float, default=0, help='Retry opening while flashing holds COM6')
args = parser.parse_args()
port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
port.dtr = port.rts = False
port.port = 'COM6'
open_deadline = time.monotonic() + args.wait_port
while True:
    try:
        port.open()
        break
    except serial.SerialException:
        if time.monotonic() >= open_deadline:
            raise
        time.sleep(0.1)
print('COM6 capture active', flush=True)
try:
    with Path(args.output).open('wb') as log:
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            data = port.read(max(port.in_waiting, 1))
            if data:
                log.write(data)
                log.flush()
finally:
    port.close()
print(args.output, flush=True)
