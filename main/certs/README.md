# Why a private key is committed here

`wu_key.pem` is a real private key, and it is in this repository on purpose.
This note exists so nobody has to guess whether it leaked. It did not.

## What it is

A self-signed certificate and key with CN `rtupdate.wunderground.com` **and a
wildcard SAN `DNS:*.wunderground.com`**, embedded into the firmware and served
by the HTTPS listener in `net/capture_server.c`. The wildcard is the part doing
the real work: `dns_server.c` spoofs every `*.wunderground.com` name, and the
host consoles actually upload to is `weatherstation.wunderground.com`.

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
protected with a password the owner sets during provisioning. The capture
endpoint is gated by `http_util_from_softap`, so a request arriving from the
home LAN is refused by the handler.

Be precise about the mechanism, because "the two sides are not routed" would be
wrong: IP forwarding and NAPT are enabled (`wifi_link.c` calls
`esp_netif_napt_enable` once the uplink is up, so AP clients get real
internet), and both listeners bind `INADDR_ANY`. A LAN host can therefore
complete a TCP or TLS connection to :80/:443. It is the handler check, not the
absence of a route, that rejects it.

More to the point: **this key is not load-bearing.** Consoles do not validate
the certificate chain, which is the only reason a self-signed cert works at
all. Anyone wanting to impersonate the spoofed WU host to a console can present
a certificate they generated themselves and it will be accepted just the same.
Possessing this key grants no capability that generating a fresh one does not.

The one thing a leaked server key can normally do is decrypt passively recorded
sessions, and that requires RSA key transport; an ECDHE handshake is forward
secret and unaffected. Either way an attacker would need to be on the WPA2
SoftAP capturing traffic in order to recover a rain total.

Scope it accurately, though. WiFi passwords are never served (`ap_pass` and
`sta_pass` are not emitted). But `GET /config` is unauthenticated for requests
arriving on the SoftAP, and it does return the STA and AP SSIDs, the AP IP, and
**the per-gauge WU upload keys in cleartext**. Since being on the SoftAP is the
same precondition this key would require, an attacker who got that far can
simply read the WU keys from `/config` rather than bother with TLS at all,
which is a further reason this key is not the interesting target.

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
  -subj "/CN=rtupdate.wunderground.com" \
  -addext "subjectAltName=DNS:rtupdate.wunderground.com,DNS:*.wunderground.com"
```

**The `-addext` line is not optional.** Consoles do not verify the chain, but
some do check the name, and a client following RFC 6125 ignores the CN entirely
when no SAN is present. Without the wildcard SAN the regenerated cert fails for
`weatherstation.wunderground.com`, the host consoles actually upload to, making
it strictly weaker than the committed one.
