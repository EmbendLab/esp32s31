# Wireless WinUSB binding (revision 0106)

The Windows query for the old 303A:4042 composite device showed that the
parent and HID MI_01 were OK; CMSIS-DAP v2 MI_00 alone was unconfigured
(user reported code 28). This establishes a driver binding failure, not
a target SWD failure. Earlier claims that setup byte order or HID descriptor
offsets were wrong were not supported by the actual code.

The new firmware uses the reference single vendor interface (EP 01/81,
512-byte bulk), VID/PID 303A:4043 and bcdDevice 0106. It advertises the
reference device-level MS OS 2.0 descriptor set (162 bytes), plus legacy
MS OS 1.0 support for virtual hosts that query string EE instead of BOS:

- String EE: MSFT100, vendor code 01.
- Vendor IN, index 4: 40-byte WINUSB compatible ID for interface zero.
- Vendor IN, index 5, value zero: 146-byte DeviceInterfaceGUIDs property.
- Vendor IN, index 7: 162-byte MS OS 2.0 descriptor set.
- Both paths use {CDB3B5AD-293B-4663-AA36-1AAE46463776}.

No Windows INF or registry edits are required by the implementation.
The new PID distinguishes this single-interface device from cached prior
composite instances. There is no wireless HID or CDC interface in this revision.

Validation: `check_usbip_descriptors.py` reads the linked ELF and verifies
the actual config, endpoint sizes, BOS UUID/length, OS 1.0/2.0 lengths,
compatible IDs, UTF-16 property name and GUID. Run it with ESP-IDF Python.
Build and flash output are saved in `results/usbip-winusb-*.log`.

At preparation time COM6 was inaccessible both inside and outside the sandbox;
the new image had not been flashed. After releasing COM6, build/flash, detach
the old USB/IP instance and attach 192.168.4.1 busid 1-1 again. Confirm the new
4043 device binds WinUSB, then run CMSIS-DAP queries and Keil download. These
Windows runtime checks are still required; descriptor validation alone does
not establish successful binding or target download.
