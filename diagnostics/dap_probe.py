"""Windows native CMSIS-DAP tests, no driver/configuration changes or packages.

Uses HID or WinUSB directly, checks bulk transfers and the advertised queue
window, and preserves/restores SRAM used for the memory test. Never erases Flash.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import struct
import time
import uuid

K = C.WinDLL('kernel32', use_last_error=True)
S = C.WinDLL('setupapi', use_last_error=True)
H = C.WinDLL('hid', use_last_error=True)
U = C.WinDLL('winusb', use_last_error=True)
PTR = C.c_void_p
DWORD = W.DWORD


class GUID(C.Structure):
    _fields_ = [('raw', C.c_ubyte * 16)]

    @classmethod
    def parse(cls, s):
        return cls((C.c_ubyte * 16).from_buffer_copy(uuid.UUID(s).bytes_le))


class INTERFACE(C.Structure):
    _fields_ = [('size', DWORD), ('guid', GUID), ('flags', DWORD), ('reserved', C.c_size_t)]


class OVERLAPPED(C.Structure):
    _fields_ = [('internal', C.c_size_t), ('internal_high', C.c_size_t),
                ('offset', DWORD), ('offset_high', DWORD), ('event', W.HANDLE)]


def signature(dll, name, result, args):
    fn = getattr(dll, name)
    fn.restype, fn.argtypes = result, args
    return fn


signature(S, 'SetupDiGetClassDevsW', PTR, [PTR, W.LPCWSTR, PTR, DWORD])
signature(S, 'SetupDiEnumDeviceInterfaces', W.BOOL, [PTR, PTR, PTR, DWORD, PTR])
signature(S, 'SetupDiGetDeviceInterfaceDetailW', W.BOOL, [PTR, PTR, PTR, DWORD, PTR, PTR])
signature(S, 'SetupDiDestroyDeviceInfoList', W.BOOL, [PTR])
signature(K, 'CreateFileW', W.HANDLE, [W.LPCWSTR, DWORD, DWORD, PTR, DWORD, DWORD, PTR])
signature(K, 'CreateEventW', W.HANDLE, [PTR, W.BOOL, W.BOOL, W.LPCWSTR])
signature(K, 'CloseHandle', W.BOOL, [W.HANDLE])
signature(K, 'WaitForSingleObject', DWORD, [W.HANDLE, DWORD])
signature(K, 'GetOverlappedResult', W.BOOL, [W.HANDLE, PTR, PTR, W.BOOL])
signature(K, 'CancelIoEx', W.BOOL, [W.HANDLE, PTR])
for name in ('ReadFile', 'WriteFile'):
    signature(K, name, W.BOOL, [W.HANDLE, PTR, DWORD, PTR, PTR])
signature(H, 'HidD_GetHidGuid', None, [PTR])
signature(H, 'HidD_FlushQueue', W.BOOL, [W.HANDLE])
signature(H, 'HidD_SetNumInputBuffers', W.BOOL, [W.HANDLE, DWORD])
signature(U, 'WinUsb_Initialize', W.BOOL, [W.HANDLE, PTR])
signature(U, 'WinUsb_Free', W.BOOL, [PTR])
signature(U, 'WinUsb_GetOverlappedResult', W.BOOL, [PTR, PTR, PTR, W.BOOL])
for name in ('WinUsb_ReadPipe', 'WinUsb_WritePipe'):
    signature(U, name, W.BOOL, [PTR, C.c_ubyte, PTR, DWORD, PTR, PTR])


def paths(guid):
    devices = S.SetupDiGetClassDevsW(C.byref(guid), None, None, 0x12)
    if devices == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    try:
        for index in range(256):
            entry = INTERFACE()
            entry.size = C.sizeof(entry)
            if not S.SetupDiEnumDeviceInterfaces(devices, None, C.byref(guid), index, C.byref(entry)):
                if C.get_last_error() == 259:
                    return
                raise C.WinError(C.get_last_error())
            size = DWORD()
            S.SetupDiGetDeviceInterfaceDetailW(devices, C.byref(entry), None, 0, C.byref(size), None)
            detail = C.create_string_buffer(size.value)
            C.cast(detail, C.POINTER(DWORD))[0] = 8 if C.sizeof(PTR) == 8 else 6
            if not S.SetupDiGetDeviceInterfaceDetailW(devices, C.byref(entry), detail, size, None, None):
                raise C.WinError(C.get_last_error())
            yield C.wstring_at(C.addressof(detail) + 4)
    finally:
        S.SetupDiDestroyDeviceInfoList(devices)


class Probe:
    def __init__(self, backend, pid=0x4041):
        self.backend = backend
        self.packet_size = 512
        self.usb = PTR()
        # The physical composite device keeps the GUID used by the original
        # working wired firmware. USB/IP has a separate PID and GUID so that
        # Windows does not reuse the composite-interface association.
        if pid in (0x4041, 0x4042):
            winusb_guid = 'cdb3f520-5a12-46d2-ab37-290cae03688d'
        else:
            winusb_guid = 'cdb3b5ad-293b-4663-aa36-1aae46463776'
        guid = GUID.parse(winusb_guid)
        if backend == 'hid':
            H.HidD_GetHidGuid(C.byref(guid))
        candidates = [p for p in paths(guid) if f'vid_303a&pid_{pid:04x}' in p.lower()]
        if len(candidates) != 1:
            raise RuntimeError(f'Expected one probe, found {candidates}')
        self.file = K.CreateFileW(candidates[0], 0xC0000000, 3, None, 3, 0x40000000, None)
        if self.file == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        if backend == 'hid':
            H.HidD_SetNumInputBuffers(self.file, 64)
            H.HidD_FlushQueue(self.file)
        elif not U.WinUsb_Initialize(self.file, C.byref(self.usb)):
            raise C.WinError(C.get_last_error())

    def io(self, reading, payload=None, timeout=5000):
        size = (513 if self.backend == 'hid' else self.packet_size) if reading else len(payload)
        buf = C.create_string_buffer(size) if reading else C.create_string_buffer(payload, size)
        done = DWORD()
        ov = OVERLAPPED()
        ov.event = K.CreateEventW(None, True, False, None)
        try:
            if self.backend == 'hid':
                ok = (K.ReadFile if reading else K.WriteFile)(self.file, buf, size, C.byref(done), C.byref(ov))
            else:
                ok = (U.WinUsb_ReadPipe if reading else U.WinUsb_WritePipe)(
                    self.usb, 0x81 if reading else 0x01, buf, size, C.byref(done), C.byref(ov))
            if not ok:
                err = C.get_last_error()
                if err != 997:
                    raise C.WinError(err)
                if K.WaitForSingleObject(ov.event, timeout) != 0:
                    K.CancelIoEx(self.file, C.byref(ov))
                    K.WaitForSingleObject(ov.event, 5000)
                    raise TimeoutError(f'{self.backend} USB timeout')
                if self.backend == 'hid':
                    ok = K.GetOverlappedResult(self.file, C.byref(ov), C.byref(done), False)
                else:
                    ok = U.WinUsb_GetOverlappedResult(self.usb, C.byref(ov), C.byref(done), False)
                if not ok:
                    raise C.WinError(C.get_last_error())
            return bytes(buf.raw[:done.value]) if reading else done.value
        finally:
            K.CloseHandle(ov.event)

    def send(self, cmd):
        cmd = bytes(cmd)
        assert len(cmd) <= self.packet_size
        if self.backend == 'hid':
            cmd = b'\0' + cmd.ljust(512, b'\0')
        assert self.io(False, cmd) == len(cmd)

    def receive(self):
        result = self.io(True)
        if self.backend == 'hid':
            assert len(result) == 513, len(result)
            return result[1:]
        return result

    def command(self, cmd):
        self.send(cmd)
        response = self.receive()
        assert response[0] == cmd[0], (bytes(cmd).hex(), response.hex())
        return response

    def xfer(self, requests):
        cmd = bytearray([5, 0, len(requests)])
        for r, value in requests:
            cmd.append(r)
            if value is not None:
                cmd += struct.pack('<I', value)
        res = self.command(cmd)
        assert res[1:3] == bytes([len(requests), 1]), res[:16].hex()
        n = sum(bool(r & 2) and not bool(r & 16) for r, _ in requests)
        return list(struct.unpack_from('<' + 'I' * n, res, 3))

    def block(self, r, count, values=None):
        cmd = bytearray([6, 0]) + struct.pack('<HB', count, r)
        if values is not None:
            cmd += struct.pack('<' + 'I' * len(values), *values)
        res = self.command(cmd)
        assert res[1:4] == struct.pack('<HB', count, 1), res[:16].hex()
        return list(struct.unpack_from('<' + 'I' * count, res, 4)) if r & 2 else []

    def connect(self):
        assert self.command([2, 1])[1] == 1
        assert self.command([0x11, 0x80, 0x84, 0x1e, 0])[1] == 0 # 2 MHz
        assert self.command([4, 2, 100, 0, 100, 0])[1] == 0
        assert self.command([0x13, 0])[1] == 0
        assert self.command(bytes([0x12, 136]) + b'\xff'*7 + b'\x9e\xe7' + b'\xff'*8)[1] == 0
        assert self.command([0x12, 8, 0])[1] == 0
        dp = self.xfer([(2, None)])[0]
        self.xfer([(0, 0x1e), (8, 0), (4, 0x50000000)])
        for _ in range(100):
            if self.xfer([(6, None)])[0] & 0xf0000000 == 0xf0000000:
                break
        else:
            raise RuntimeError('Debug power-up timed out')
        self.xfer([(1, 0x23000052)])
        return dp

    def read(self, address, count=1, block=False):
        # MEM-AP auto increment wraps at 1 KiB on this target.
        values = []
        while count:
            n = min(count, 120, (0x400 - (address & 0x3ff)) // 4)
            self.xfer([(8, 0), (1, 0x23000052), (5, address)])
            values += self.block(15, n) if block else self.xfer([(15, None)] * n)
            address += n * 4
            count -= n
        return values

    def write(self, address, values, block=False):
        values = list(values)
        while values:
            n = min(len(values), 100, (0x400 - (address & 0x3ff)) // 4)
            self.xfer([(8, 0), (1, 0x23000052), (5, address)])
            if block:
                self.block(13, n, values[:n])
            else:
                self.xfer([(13, v) for v in values[:n]])
            address += 4 * n
            values = values[n:]

    def close(self):
        if self.usb:
            U.WinUsb_Free(self.usb)
        K.CloseHandle(self.file)


def run(backend):
    p = Probe(backend)
    report = {'backend': backend}
    try:
        report['firmware'] = p.command([0, 9])[2:].split(b'\0')[0].decode()
        window = p.command([0, 0xfe])[2]
        report['packet_count'] = window
        report['dpidr'] = hex(p.connect())
        assert p.block(2, 3) == [int(report['dpidr'], 16)] * 3
        report['block_dp_read'] = 'PASS'
        for _ in range(20):
            ids = [1, 2, 3, 4, 7, 8, 9, 0xfe][:window]
            expected = [p.command([0, info]) for info in ids]
            for info in ids:
                p.send([0, info])
            for info, wanted in zip(ids, expected):
                answer = p.receive()
                assert answer == wanted, (info, answer[:40])
        report['queued_responses'] = 'PASS (20 windows)'
        p.xfer([(8, 0xf0)])
        base = p.xfer([(11, None)])[0] & ~3
        p.xfer([(8, 0)])
        report['rom_base'] = hex(base)
        for use_block in (False, True):
            pid = p.read(base + 0xfe0, 3, block=use_block)
            assert ((pid[2] & 7) << 4) | ((pid[1] >> 4) & 15) == 0x20, pid
        report['keil_check_id'] = 'PASS (Transfer and TransferBlock)'
        report['cpuid'] = hex(p.read(0xe000ed00)[0])
        dhcsr = p.read(0xe000edf0)[0]
        p.write(0xe000edf0, [0xa05f0003])
        for _ in range(100):
            if p.read(0xe000edf0)[0] & (1 << 17):
                break
        else:
            raise RuntimeError('Target did not halt')
        original = p.read(0x20000000, 256)
        backup = Path(__file__).parent / 'results' / f'sram-before-{backend}.bin'
        backup.write_bytes(struct.pack('<256I', *original))
        try:
            pattern = [((i * 0x9e3779b9) ^ 0xa55a5aa5) & 0xffffffff for i in range(256)]
            p.write(0x20000000, pattern, block=True)
            assert p.read(0x20000000, 256) == pattern
            assert p.read(0x20000000, 256, block=True) == pattern
            p.write(0x20000000, [v ^ 0xffffffff for v in pattern])
            assert p.read(0x20000000, 256, block=True) == [v ^ 0xffffffff for v in pattern]
            report['sram_write_read_1024_bytes'] = 'PASS (both commands)'
        finally:
            p.write(0x20000000, original)
            assert p.read(0x20000000, 256) == original
            if not dhcsr & (1 << 17):
                p.write(0xe000edf0, [0xa05f0000 | (dhcsr & 1)])
        p.command([3])
        return report
    finally:
        p.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--backend', choices=['hid', 'bulk'], required=True)
    args = parser.parse_args()
    result = run(args.backend)
    out = Path(__file__).parent / 'results' / f'protocol-{args.backend}.json'
    out.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result, indent=2))
