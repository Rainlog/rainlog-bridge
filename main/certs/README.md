# Why a private key is committed here

`wu_key.pem` is a real private key, and it is in this repository on purpose.
This note exists so nobody has to guess whether it leaked. It did not.

## What it is

A self-signed certificate and key with CN `rtupdate.wunderground.com`, embedded
into the firmware and served by the HTTPS listener in `net/capture_server.c`.

The bridge impersonates Weather Underground on its own SoftAP: `net/dns_server.c`
answers `*.wunderground.com` with the bridge's AP address, so a personal weather
station console uploading to WU reaches the bridge instead. Most consoles use
plain HTTP, but some (AcuRite, for one) insist on TLS. They do not validate the
certificate chain, so a self-signed cert is accepted and the upload can be
captured.

Generating this on-device would be the obvious alternative. It is not done yet
(see below), so the key ships with the image and is therefore shared by every
unit running the same firmware.

## What its exposure does and does not mean

Using this key requires being **on the bridge's SoftAP**, which is WPA2
protected with a password the owner sets during provisioning. There is no route
to it from the home LAN or the internet: the bridge does not route between the
two sides, and the capture endpoint is gated by `http_util_from_softap`.

An attacker who has already joined that SoftAP could impersonate the spoofed WU
host to a console. What that yields is one weather station's upload: rain
readings, and the WU station credentials the console was configured with. It
does not expose the bridge's own configuration, the owner's home WiFi password,
or any Rainlog account credential. Those live in NVS and are never served.

So: low impact, but genuinely a shared secret rather than a per-device one.

## Planned fix

Generate the certificate and key on first boot into LittleFS, so each unit gets
its own and nothing secret ships in the image. Tracked as an issue.

## Regenerating

```sh
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout wu_key.pem -out wu_cert.pem \
  -subj "/CN=rtupdate.wunderground.com"
```

The CN must stay `rtupdate.wunderground.com`. Consoles do not verify the chain,
but some do check the name.
