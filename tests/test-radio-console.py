#!/usr/bin/env python3
"""Exercise radio console controls on a debug build.

Pass the intended serial device explicitly. Resets that device, then restores
its default tuning and receive state. Requires pyserial and esptool.
"""
import sys
import time

import serial
from esptool.reset import HardReset

with serial.Serial(sys.argv[1], 115200, timeout=0.1) as port:
    port.setDTR(False)
    HardReset(port)()
    boot = bytearray()
    # Boot includes Wi-Fi startup; console evaluation has its own 200 ms limit.
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        boot.extend(port.read(port.in_waiting or 1))
        if b'Debug console ready' in boot:
            break
    else:
        raise RuntimeError('Console did not become ready')

    def command(source, expected='OK'):
        port.reset_input_buffer()
        port.write(('js ' + source + '\n').encode())
        response = bytearray()
        # Allow serial scheduling while the firmware enforces evaluation limits.
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            response.extend(port.read(port.in_waiting or 1))
            if b'Result=' in response:
                break
        text = response.decode(errors='replace')
        print(source, text, flush=True)
        assert 'Result=' + expected in text, text

    command('radio.receive(false)')
    command('if(radio.status().receiving)throw Error("not paused");')
    command('radio.tune(433930000,250000,15)')
    command('radio.receive(true)')
    command('if(!radio.status().receiving || '
            'radio.status().frequency_hz!==433930000)throw Error("not tuned");')
    command('radio.tune(915000000,250000,15)', 'ERROR')
    command('radio.receive("yes")', 'ERROR')
    command('radio.tune(433920000,250000,15)')
    command('if(!web.radio().available)throw Error("web radio unavailable");')
    command('print(JSON.stringify(radio.status()))')
    command('print(JSON.stringify(hw.heap()))')
