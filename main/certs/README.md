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

The key ships inside the image, so every unit running a given build shares it.
That is fine here, for the reason below.

## What its exposure does and does not mean

Using this key requires being **on the bridge's SoftAP**, which is WPA2
protected with a password the owner sets during provisioning. There is no route
to it from the home LAN or the internet: the bridge does not route between the
two sides, and the capture endpoint is gated by `http_util_from_softap`.

More to the point: **this key is not load-bearing.** Consoles do not validate
the certificate chain, which is the only reason a self-signed cert works at
all. Anyone wanting to impersonate the spoofed WU host to a console can present
a certificate they generated themselves and it will be accepted just the same.
Possessing this key grants no capability that generating a fresh one does not.

The one thing a leaked server key can normally do is decrypt passively recorded
sessions, and that requires RSA key transport; an ECDHE handshake is forward
secret and unaffected. Either way an attacker would need to be on the WPA2
SoftAP capturing traffic in order to recover a rain total.

Worth being explicit that a compromise here does not reach anything else: the
bridge's own configuration, the owner's home WiFi password, and Rainlog
credentials live in NVS and are never served.

## Why this is not "fixed" by generating it on-device

Per-device key generation is the reflexive answer, and it buys nothing here,
because key extraction is not what an attacker needs (see above). It would cost
real things: RSA-2048 keygen is slow on a C6, heap is already tight with AP+STA
WiFi plus the LCD framebuffer plus TLS, and a keygen failure would mean no
HTTPS listener at all, breaking the very consoles that listener exists for.

The certificate here is a compatibility shim to complete a handshake, not a
security boundary. Treating it as one would make the firmware worse.

## Regenerating

```sh
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout wu_key.pem -out wu_cert.pem \
  -subj "/CN=rtupdate.wunderground.com"
```

The CN must stay `rtupdate.wunderground.com`. Consoles do not verify the chain,
but some do check the name.
