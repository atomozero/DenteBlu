# Haiku Bug Ticket

**Title:** XHCI: ConfigureEndpoint rejects isochronous endpoints with wMaxPacketSize=0

**Component:** Drivers/USB

---

Every boot I get two of these from my BT adapter (Intel AX201, 8087:0026):

```
usb error xhci 1: ConfigureEndpoint() failed invalid max_burst_payload
usb error xhci 1: unable to configure endpoint: Invalid Argument
```

It's the isochronous endpoints on BT Interface 1 Alt 0 which have wMaxPacketSize=0 — standard zero-bandwidth placeholder per USB spec, you switch to a higher alt for actual SCO audio. But xhci.cpp ~line 2277 computes `max_burst_payload = (maxBurst+1) * maxPacketSize`, gets 0, and returns B_BAD_VALUE.

XHCI 1.2 § 4.14.1.1 allows Max Packet Size = 0 for these. Same thing would happen with USB Audio devices that also use zero-bandwidth Alt 0.

Harmless for BT (h2generic only uses Interface 0) but probably breaks USB Audio isoc setup. Patch attached — lets isoc endpoints through with max_burst_payload=1 to avoid div-by-zero in TRB math, guards AVGTRBLENGTH/MAXESITPAYLOAD writes.

Same errors visible in #17042 syslog.
