#!/usr/bin/env python3
"""LILYGO radio settings integration test, selected serial device is required.

Temporarily saves a test mapping and disables Wi-Fi interception while retaining radio reception, verifies
persistence and LAN setup access after reboot, then restores original settings.
Requires pyserial/esptool and configured home Wi-Fi. No weather uploads are sent.
"""
import base64
import json
import re
import sys
import time
import urllib.request
import urllib.parse

import serial
from esptool.reset import HardReset

with serial.Serial(sys.argv[1], 115200, timeout=0.1) as port:
    def boot():
        port.setDTR(False)
        port.reset_input_buffer()
        HardReset(port)()
        output = bytearray()
        # Wi-Fi startup precedes the console ready message.
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            output.extend(port.read(port.in_waiting or 1))
            if b'Debug console ready' in output:
                return output.decode(errors='replace')
        raise RuntimeError('Console did not start')

    def command(source):
        port.reset_input_buffer()
        port.write(('js ' + source + '\n').encode())
        output = bytearray()
        # Native management runs on the HTTP task; allow serial scheduling.
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            output.extend(port.read(port.in_waiting or 1))
            if b'Result=' in output:
                text = output.decode(errors='replace')
                assert 'Result=OK' in text, 'Console command failed'
                return text
        raise RuntimeError('No console response')

    def value(source):
        result = command('print("VALUE="+JSON.stringify(' + source + '))')
        return json.loads(re.search(r'VALUE=([^\r\n]+)', result).group(1))

    original = None
    try:
        boot()
        original = value('(function(){var c=settings.get(); return '
                         '{radio_map:c.radio_map,radio_enabled:c.radio_enabled,'
                         'wifi_interception_enabled:c.wifi_interception_enabled};})()')
        password = value('settings.get().ap_pass')
        patch = {'radio_enabled': True, 'wifi_interception_enabled': False,
                 'radio_map': [{'model': 0, 'sensor_id': 7, 'channel': 0,
                                'gauge_id': 4294967294, 'rainlog_key': 'test-only-key'}]}
        command('settings.set(' + json.dumps(patch) + ')')
        output = boot()
        assert 'HTTPS capture server up' not in output
        assert 'mode : sta (' in output and 'softAP' not in output
        saved = value('settings.get().radio_map')
        assert saved == patch['radio_map']
        assert value('radio.status().receiving')
        # The configured LAN can take several seconds to reconnect after reboot.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            wifi = value('hw.wifi()')
            if wifi['connected']:
                break
            time.sleep(0.2)
        else:
            raise RuntimeError('Home Wi-Fi did not reconnect')
        request = urllib.request.Request('http://' + wifi['sta'] + '/config')
        request.add_header('Authorization', 'Basic ' + base64.b64encode(('bridge:' + password).encode()).decode())
        # A local setup request should finish in seconds, even during Wi-Fi sleep.
        with urllib.request.urlopen(request, timeout=10) as response:
            config = json.load(response)
        assert not config['wifi_interception_enabled'] and config['radio_enabled']
        assert config['radio_map'] == patch['radio_map']
        # Exercise real form parsing without saving: duplicate valid rows must
        # be rejected even with a non-default setup password.
        body = {'radio_form': '2', 'radio_enabled': 'on',
                'wifi_interception_enabled': 'on', 'sta_ssid': config['sta_ssid'],
                'ap_ssid': config['ap_ssid'], 'ap_pass': 'parser-test-password'}
        for index in range(2):
            body.update({f'radio_model{index}': '0', f'radio_id{index}': '7',
                         f'radio_gauge{index}': 'Rainlog12345',
                         f'radio_key{index}': 'parser-test-key'})
        invalid = urllib.request.Request('http://' + wifi['sta'] + '/save',
                                         data=urllib.parse.urlencode(body).encode())
        invalid.add_header('Authorization', request.get_header('Authorization'))
        with urllib.request.urlopen(invalid, timeout=10) as response:
            assert b'duplicate radio sensor or gauge id' in response.read()
        assert value('settings.get().radio_map') == patch['radio_map']
        print('HTTP mapping form validation passed without changing saved settings')
        print('Radio mapping persistence, paused reception, STA-only boot and authenticated LAN setup passed')
    finally:
        if original is not None:
            command('settings.set(' + json.dumps(original) + ')')
            boot()
            print('Original mappings and reception settings restored')
