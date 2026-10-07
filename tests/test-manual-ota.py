#!/usr/bin/env python3
"""Live manual OTA checks on the explicitly selected LILYGO debug device.

Reads its password privately from the console. Checks unauthorized, wrong-board,
interrupted and corrupt uploads retain the current boot slot, then installs the
supplied LILYGO app image and verifies the new slot passes its rollback health
check. Does not change settings or access other serial devices.
Usage: test-manual-ota.py SERIAL_DEVICE LILYGO_APP_BIN C6_APP_BIN
"""
import base64
import http.client
import json
import re
import socket
import sys
import time
from pathlib import Path
import serial
from esptool.reset import HardReset

image = Path(sys.argv[2]).read_bytes()
other = Path(sys.argv[3]).read_bytes()
with serial.Serial(sys.argv[1], 115200, timeout=0.1) as port:
    def until(marker):
        output = bytearray()
        # The local board normally boots in a few seconds, including Wi-Fi.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            output.extend(port.read(port.in_waiting or 1))
            if marker in output:
                return output.decode(errors='replace')
        raise RuntimeError('Expected console response did not arrive')

    def value(source):
        port.reset_input_buffer()
        port.write(('js print("VALUE="+JSON.stringify(' + source + '))\n').encode())
        output = until(b'Result=')
        assert 'Result=OK' in output, 'Console command failed'
        return json.JSONDecoder().raw_decode(re.search(r'^VALUE=([^\r\n]+)', output, re.M).group(1))[0]

    port.setDTR(False)
    HardReset(port)()
    until(b'Debug console ready')
    assert value('hw.buildInfo().board') == 'lilygo-t3-v1.6.1-sx1278'
    password = value('settings.get().ap_pass')
    deadline = time.monotonic() + 15  # Wi-Fi association follows console startup.
    while True:
        wifi = value('hw.wifi()')
        if wifi['connected']: break
        assert time.monotonic() < deadline, 'Configured Home Wi-Fi is required'
        time.sleep(0.2)
    host = wifi['sta']
    auth = 'Basic ' + base64.b64encode(('bridge:' + password).encode()).decode()

    def request(path, body=None, authenticated=True, intent=True):
        # Allow the HTTP server's receive timeout plus flash erase/verification.
        conn = http.client.HTTPConnection(host, timeout=30)
        headers = {'Authorization': auth} if authenticated else {}
        if body is not None:
            headers['Content-Type'] = 'application/octet-stream'
            if intent: headers['X-Rainlog-OTA'] = '1'
        conn.request('POST' if body is not None else 'GET', path, body, headers)
        response = conn.getresponse()
        status, data = response.status, response.read()
        conn.close()
        return status, data

    def status():
        code, data = request('/ota/status')
        assert code == 200
        return json.loads(data)

    # Boot health validation takes 20 seconds; the initial manifest check can
    # also be holding the shared operation lock during its network timeout.
    deadline = time.monotonic() + 45
    while True:
        before = status()
        if before['phase'] not in ('checking', 'updating') and not before['pending_verification']:
            break
        assert time.monotonic() < deadline, 'OTA did not become idle and healthy'
        time.sleep(0.2)
    assert before['boot_slot'] == before['running_slot']

    def retained():
        after = status()
        assert after['boot_slot'] == before['boot_slot']
        assert after['running_slot'] == before['running_slot']

    def rejected(body, expected, **kwargs):
        try:
            assert request('/ota/upload', body, **kwargs)[0] == expected
        except (ConnectionResetError, http.client.RemoteDisconnected):
            # Early rejection closes the socket without consuming binary input.
            pass

    rejected(image[:336], 401, authenticated=False)
    rejected(image[:336], 400, intent=False)
    rejected(other[:336], 400)
    retained()
    print('Authentication, upload intent and wrong-board rejection passed', flush=True)

    sock = socket.create_connection((host, 80), timeout=30)
    header = ('POST /ota/upload HTTP/1.1\r\nHost: ' + host + '\r\nAuthorization: ' + auth +
              '\r\nContent-Type: application/octet-stream\r\nX-Rainlog-OTA: 1\r\nContent-Length: ' +
              str(len(image)) + '\r\nConnection: close\r\n\r\n')
    sock.sendall(header.encode() + image[:1024])
    sock.shutdown(socket.SHUT_WR)
    response = bytearray()
    while chunk := sock.recv(4096): response.extend(chunk)
    sock.close()
    assert b'400 Bad Request' in response
    retained()
    print('Interrupted upload retained the old boot slot', flush=True)

    corrupted = bytearray(image)
    corrupted[-1] ^= 1
    code, data = request('/ota/upload', corrupted)
    assert code == 400 and b'verification failed' in data
    retained()
    print('Corrupt image rejected after full upload; old boot slot retained', flush=True)

    code, data = request('/ota/upload', image + b'junk')
    assert code == 400 and b'verification failed' in data
    retained()
    print('Trailing data rejected; old boot slot retained', flush=True)

    port.reset_input_buffer()
    code, data = request('/ota/upload', image)
    assert code == 200 and json.loads(data)['ok']
    until(b'Debug console ready')
    deadline = time.monotonic() + 15  # Wi-Fi association follows console startup.
    while not value('hw.wifi()')['connected']:
        assert time.monotonic() < deadline, 'Home Wi-Fi did not reconnect'
        time.sleep(0.2)
    deadline = time.monotonic() + 30  # Firmware marks healthy after 20 seconds.
    while True:
        after = status()
        assert after['running_slot'] != before['running_slot']
        if not after['pending_verification']:
            assert after['boot_slot'] == after['running_slot']
            break
        assert time.monotonic() < deadline, 'New firmware did not pass boot health check'
        time.sleep(0.2)
    print('Valid manual upload booted the inactive slot and passed rollback health check', flush=True)
