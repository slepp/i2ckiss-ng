# Migrating from the original i2ckiss

For a new installation, follow the single-TNC instructions in the
[README](../README.md). This guide is for systems already running the original
`i2ckiss` daemon.

The migration replaces the old startup command with a systemd instance and
gives the KISS application a stable device path under `/run`.

## Prepare

1. Back up the original executable, its startup command, the KISS application
   configuration, and the current device symlink.
2. Install `i2ckiss-ng`.
3. Create `/etc/i2ckiss/tnc0.conf` with the existing I2C bus and address.

```ini
I2CKISS_OPTIONS="1 0x10"
```

If the system has multiple TNCs, create one instance per address as described
in [multiple-tncs.md](multiple-tncs.md).

## Replace the daemon

1. Stop APRX, `kissattach`, or the other KISS application.
2. Stop the original `i2ckiss` process.
3. Remove its launch command from `/etc/rc.local`, cron, or another startup
   script.
4. Enable the new instance:

```sh
sudo systemctl enable --now i2ckiss@tnc0.service
```

5. Change the KISS application device to:

```text
/run/i2ckiss-tnc0/pty
```

6. Remove the old device symlink and start the KISS application.

To retain the old `/dev` name, create a system-managed alias from
[device-aliases.md](device-aliases.md).

## Verify

```sh
systemctl status i2ckiss@tnc0.service
readlink /run/i2ckiss-tnc0/pty
journalctl -u i2ckiss@tnc0.service
```

The service should report `I2C connected`, and the KISS application should
open `/run/i2ckiss-tnc0/pty` or its optional `/dev` alias.
