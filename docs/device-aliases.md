# Optional permanent device aliases

A normal installation can point its KISS application directly at the stable
service path:

```text
/run/i2ckiss-tnc0/pty
```

A `/dev` alias provides a particular device name such as `/dev/aprs-i2c` for
applications and existing configurations that use one.

## About `/etc/tmpfiles.d`

`/etc/tmpfiles.d/` is the standard systemd directory for permanent local rules
that create runtime files, directories, and links. The configuration file in
`/etc` is persistent. The name “tmpfiles” refers to the objects it manages:
paths under `/run` and `/dev` may be recreated at boot.

This gives i2ckiss-ng a permanent public name for the service path.

## Create an alias

Create `/etc/tmpfiles.d/i2ckiss.conf`:

```text
L /dev/i2ckiss-tnc0 - - - - /run/i2ckiss-tnc0/pty
```

Apply the rule immediately:

```sh
sudo systemd-tmpfiles --create /etc/tmpfiles.d/i2ckiss.conf
```

Applications may now use `/dev/i2ckiss-tnc0`. The complete path is:

```text
/dev/i2ckiss-tnc0 -> /run/i2ckiss-tnc0/pty
```

The alias is created from permanent system configuration and points to the
device published by the running service.

Additional aliases can be added as additional `L` lines in the same file.
