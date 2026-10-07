#!/usr/bin/env python3
"""LILYGO management smoke test: device and serial log path are explicit.

Temporarily changes display dim brightness, checks it across a reboot, then
restores it. Does not factory reset, install OTA, or change network credentials.
"""
import json
import re
import sys
import time
import serial
from esptool.reset import HardReset

log = bytearray()
with serial.Serial(sys.argv[1], 115200, timeout=0.1) as port:
    def until(pattern):
        start = len(log)
        # Boot, scans and queued work need scheduling time; JS itself has 200 ms.
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            log.extend(port.read(port.in_waiting or 1))
            response = re.sub(r'\x1b\[[0-9;]*m', '', log[start:].decode(errors='replace'))
            if re.search(pattern, response):
                return response
        raise RuntimeError('Missing ' + pattern + '\n' + response)

    def command(source, success=True, contains=None):
        port.write(('js ' + source + '\n').encode())
        response = until('Result=' + ('OK' if success else 'ERROR'))
        if contains is not None:
            assert contains in response, response
        print(source, response.strip())
        return response

    def value(source):
        response = command('print("VALUE="+JSON.stringify(' + source + '))')
        return json.loads(re.search(r'VALUE=([^\r\n]+)', response).group(1))

    original = None
    changed = False
    try:
        port.setDTR(False)
        HardReset(port)()
        until('Debug console ready')
        value('Object.keys(settings.get())')
        original = value('settings.get().display_dim_pct')
        provisioned = value('settings.get().provisioned')
        value('hw.buildInfo()')
        value('web.config().ap_ip')
        value('web.clients()')
        value('web.scan()')
        value('web.scanLive()')
        value('web.testStatus()')
        value('web.otaStatus()')
        command('web.call("settings.set")', False, 'unknown web action')
        command('settings.set({typo:1})', False, 'unknown or read-only')
        command('settings.set({display_full_pct:101})', False, 'out of range')
        command('settings.set({ap_ssid:new Array(35).join("x")})', False, 'too long')
        command('settings.set({ota_host:"https://bad.example"})', False, 'hostname')
        command('settings.set({wu_update_path:"invalid"})', False, 'paths')
        command('settings.set({wu_map:[{gauge_id:1,wu_id:"TEST",wu_key:""},{gauge_id:1,wu_id:"TEST",wu_key:""}]})', False, 'duplicate')
        command('web.save("ap_pass=short")', False, 'at least 8')
        command('web.rename("bad", "test")', False)
        command('web.rename("02:00:00:FF:FF:FE", "console-test")')
        command('web.rename("02:00:00:FF:FF:FE", "")')
        command('hw.logLevel("debug",0); 42', contains='42')
        command('hw.logLevel("debug",3); 42', contains='42')
        target = 7 if original != 7 else 8
        command('settings.set({display_dim_pct:' + str(target) + '})')
        changed = True
        assert value('settings.get().display_dim_pct') == target
        assert value('settings.get().provisioned') == provisioned
        command('web.reboot()')
        until('Debug console ready')
        assert value('settings.get().display_dim_pct') == target
        assert value('settings.get().provisioned') == provisioned
        command('settings.set({display_dim_pct:' + str(original) + '})')
        changed = False
        assert value('settings.get().display_dim_pct') == original
        command('hw.wake(); 42', contains='42')
        value('Object.keys(web)')
        value('hw.heap()')
        assert b'Guru Meditation' not in log and b'Duktape fatal' not in log
        print('PASS: management settings persisted and restored')
    finally:
        if changed and original is not None:
            command('settings.set({display_dim_pct:' + str(original) + '})')
        with open(sys.argv[2], 'wb') as output:
            output.write(log)
