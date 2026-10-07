#!/usr/bin/env python3
"""Serial integration test. Pass the intended device and log path explicitly.

Requires pyserial and esptool, available in the ESP-IDF container.
Resets the selected board and exercises read-only bindings, limits and recovery.
"""
import re
import sys
import time
import serial
from esptool.reset import HardReset

log = bytearray()
with serial.Serial(sys.argv[1], 115200, timeout=0.1) as port:
    port.setDTR(False)
    HardReset(port)()

    def until(pattern):
        # Ten seconds allows Wi-Fi boot scheduling; eval itself is capped at 200 ms.
        start = len(log)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            log.extend(port.read(port.in_waiting or 1))
            response = log[start:].decode(errors='replace')
            if re.search(pattern, response):
                return response
        raise RuntimeError('Missing response: ' + pattern + '\n' + response)

    def command(source, success=True, contains=None):
        port.write(('js ' + source + '\n').encode())
        response = until('Result=' + ('OK' if success else 'ERROR'))
        if contains is not None:
            assert contains in response, response
        print(source, re.sub(r'\x1b\[[0-9;]*m', '', response).strip())

    try:
        until('Debug console ready')
        command('1+2', contains='3\r\n')
        command('var debugValue=37; debugValue', contains='37')
        command('debugValue+5', contains='42')
        command('JSON.stringify(hw.heap())', contains='"vm"')
        command('JSON.stringify(hw.wifi())', contains='"connected"')
        command('JSON.stringify(hw.stats())', contains='"pending"')
        command('hw.gpio(0)')
        command('hw.gpio(-1)', False, 'invalid GPIO')
        command('6*7', contains='42')
        command('throw new Error("test error")', False, 'test error')
        command('6*7', contains='42')
        command('while(true){}', False)
        command('7*8', contains='56')
        command('new Array(1000000).join("x")', False)
        command('8*8', contains='64')
        command('Object.isExtensible(Array.prototype)', contains='false')
        command('"use strict"; Array.prototype.rainlogTest=1', False)
        command('off')
        command('9*9', contains='VM startup=')
        port.write(b'js ' + b'x'*2100 + b'\n')
        until('discarded')
        command('10*10', contains='100')
        port.write(b'js 1\x00+2\n')
        until('discarded')
        command('JSON.stringify(hw.heap())', contains='"vm"')
        assert b'Guru Meditation' not in log and b'Duktape fatal' not in log
        print('PASS')
    finally:
        with open(sys.argv[2], 'wb') as output:
            output.write(log)
