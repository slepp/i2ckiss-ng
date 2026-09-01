# Running more than one TNC

Each TNC uses its own systemd instance, configuration file, I2C address, and
KISS device path. Multiple TNCs may share the same I2C bus when each has a
different address.

For example, create an APRS instance at address `0x66`:

```ini
# /etc/i2ckiss/aprs.conf
I2CKISS_OPTIONS="1 0x66"
```

Create a packet instance at address `0x65`:

```ini
# /etc/i2ckiss/packet.conf
I2CKISS_OPTIONS="1 0x65"
```

Enable both instances:

```sh
sudo systemctl enable --now i2ckiss@aprs.service
sudo systemctl enable --now i2ckiss@packet.service
```

The corresponding KISS device paths are:

```text
/run/i2ckiss-aprs/pty
/run/i2ckiss-packet/pty
```

Configure each KISS application with the path for its TNC. If legacy `/dev`
names are needed, add one rule per instance as described in
[device-aliases.md](device-aliases.md).

Check each bridge independently:

```sh
systemctl status i2ckiss@aprs.service
systemctl status i2ckiss@packet.service
journalctl -u i2ckiss@aprs.service
journalctl -u i2ckiss@packet.service
```
