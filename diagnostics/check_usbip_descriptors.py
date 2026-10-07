"""Validate the actual linked wireless descriptors; uses ESP-IDF's pyelftools."""
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]


def u16(data, offset):
    return struct.unpack_from('<H', data, offset)[0]


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


with (ROOT / 'build/softAP.elf').open('rb') as stream:
    elf = ELFFile(stream)
    symbols = elf.get_section_by_name('.symtab')

    def read(name):
        sym, = symbols.get_symbol_by_name(name)
        section = elf.get_section(sym['st_shndx'])
        offset = sym['st_value'] - section['sh_addr']
        return section.data()[offset:offset + sym['st_size']]

    device = read('g_dap_usbip_device_desc')
    assert u16(device, 10) == 0x4043
    assert u16(device, 12) == 0x0106
    config = read('g_dap_usbip_config_desc')
    assert u16(config, 2) == len(config) == 39 and config[4] == 1
    assert config[9:18] == bytes([9, 4, 0, 0, 3, 255, 0, 0, 0])
    assert config[20] == 1 and config[27] == 0x81 and config[34] == 0x82
    assert u16(config, 22) == u16(config, 29) == u16(config, 36) == 512

    bos = read('s_usbip_bos_desc')
    ms2 = read('s_usbip_ms_os_20_desc')
    assert u16(bos, 2) == len(bos) == 33
    assert bos[9:25].hex() == 'df60ddd88945c74c9cd2659d9e648a9f'
    assert u16(bos, 29) == u16(ms2, 8) == len(ms2) == 162
    assert u16(ms2, 10) == 20 and u16(ms2, 12) == 3
    assert ms2[14:22] == b'WINUSB\0\0'
    assert u16(ms2, 30) == len(ms2) - 30 == 132
    assert u16(ms2, 34) == 7 and u16(ms2, 36) == 42
    assert ms2[38:80].decode('utf-16le') == 'DeviceInterfaceGUIDs\0'
    guid = '{CDB3B5AD-293B-4663-AA36-1AAE46463776}\0\0'
    assert u16(ms2, 80) == 80 and ms2[82:].decode('utf-16le') == guid

    string = read('s_usbip_ms_os_10_string')
    assert string[:2] == bytes([18, 3])
    assert string[2:16].decode('utf-16le') == 'MSFT100'
    assert string[16] == bos[31] == 1
    compat = read('s_usbip_ms_os_10_compat')
    assert u32(compat, 0) == len(compat) == 40
    assert u16(compat, 6) == 4 and compat[8] == 1
    assert compat[16:26] == b'\0\1WINUSB\0\0'
    prop = read('s_usbip_ms_os_10_properties')
    assert u32(prop, 0) == len(prop) == 146
    assert u16(prop, 6) == 5 and u16(prop, 8) == 1
    assert u32(prop, 10) == 136 and u32(prop, 14) == 7
    assert u16(prop, 18) == 42
    assert prop[20:62].decode('utf-16le') == 'DeviceInterfaceGUIDs\0'
    assert u32(prop, 62) == 80 and prop[66:].decode('utf-16le') == guid

print('PASS: linked USB config, BOS, MS OS 1.0/2.0 lengths, WINUSB IDs and DAP GUID')
