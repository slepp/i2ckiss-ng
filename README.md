# i2ckiss-ng

[![Build](https://github.com/slepp/i2ckiss-ng/actions/workflows/build.yml/badge.svg)](https://github.com/slepp/i2ckiss-ng/actions/workflows/build.yml)

`i2ckiss-ng` presents a TNC-Pi or TNC-Black I2C modem as a Linux KISS serial
PTY for applications such as APRX. It runs continuously under systemd and
supports direct attachment to the Linux AX.25 stack.

It replaces the original `i2ckiss` utility while retaining the installed
executable name and compatible command-line forms.

## Install one TNC

Most installations need one service instance and one device path.

### 1. Install the package

On a 32-bit Raspberry Pi OS system, download the `armhf` package from the
[latest release](https://github.com/slepp/i2ckiss-ng/releases/latest), then run:

```sh
sudo apt install ./i2ckiss-ng_*_armhf.deb
```

For another architecture, use its matching package or build from source:

```sh
make
make test
sudo make install
sudo systemctl daemon-reload
```

The I2C device must already be enabled on the system. For bus 1, this is
`/dev/i2c-1`.

### 2. Configure the TNC

Create one instance named `tnc0`:

```sh
sudo cp /etc/i2ckiss/example.conf /etc/i2ckiss/tnc0.conf
sudoedit /etc/i2ckiss/tnc0.conf
```

Set the I2C bus number and the address configured on the TNC. This example uses
bus 1 and address `0x10`:

```ini
I2CKISS_OPTIONS="1 0x10"
```

### 3. Start the bridge

```sh
sudo systemctl enable --now i2ckiss@tnc0.service
systemctl status i2ckiss@tnc0.service
```

A working service reports `I2C connected`. Its KISS device is:

```text
/run/i2ckiss-tnc0/pty
```

Use that path in APRX, Dire Wolf, or any other KISS application. The path stays
the same across service restarts.

### 4. Configure APRX

In the APRX interface stanza, use the i2ckiss-ng device path:

```text
<interface>
   serial-device /run/i2ckiss-tnc0/pty 9600 8n1 KISS
   callsign $mycall
</interface>
```

Keep the callsign, speed, and transmit settings appropriate for the existing
APRX configuration, then restart APRX.

To make APRX start after the bridge, run `sudo systemctl edit aprx.service` and
add:

```ini
[Unit]
Requires=i2ckiss@tnc0.service
After=i2ckiss@tnc0.service
```

Useful logs:

```sh
journalctl -u i2ckiss@tnc0.service
journalctl -u aprx.service
```

## More configurations

- [Add a second or additional TNC](docs/multiple-tncs.md)
- [Create an optional permanent `/dev` alias](docs/device-aliases.md)
- [Migrate from the original i2ckiss](docs/migration.md)

The `/run/i2ckiss-tnc0/pty` path is the standard endpoint for a normal
installation. Optional `/dev` aliases support applications and existing
configurations that use a particular device name.

## Manual usage

Run without systemd and publish a KISS PTY symlink:

```sh
sudo i2ckiss 1 0x10 symlink /run/i2ckiss-tnc0
```

Attach directly to the Linux AX.25 stack through `kissattach`:

```sh
sudo i2ckiss 1 0x10 1 10.1.1.1
```

Bus names may be a number such as `1` or a complete path such as
`/dev/i2c-1`. I2C addresses accept decimal or `0x` notation. Run
`i2ckiss --help` for polling, retry, reset, logging, and daemon options.

## Features

- I2C TNC to KISS serial PTY bridge
- One or more independently configured TNC instances
- systemd service management, logging, and recovery
- Direct `kissattach` mode for the Linux AX.25 stack
- Command-line compatibility with the original `i2ckiss`

## Build distribution packages

```sh
make packages
```

This builds a versioned source archive and native plus ARM hard-float Debian
packages under `dist/`, along with `SHA256SUMS`. The package build uses
`dpkg-deb` and the `arm-linux-gnueabihf` GCC toolchain. Release packages are
built in Debian 11 (Bullseye) for compatibility with established Raspberry Pi
systems.

## Compatibility and lineage

This project replaces the `i2ckiss` program formerly distributed for TNC-Pi
systems and described on the
[K4GBB TNC-Pi setup page](http://k4gbb.us/docs/tncpi.html). The original
utility is attributed to John Wiseman, G8BPQ. `i2ckiss-ng` is an independent
implementation of its public interface and TNC-Pi wire behavior.

## License

Copyright (c) 2026 Stephen Olesen.

Released under the [MIT License](LICENSE).
