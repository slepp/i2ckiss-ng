# Changelog

## 2.0.1 - 2026-09-05

- Simplify frame decoding, PTY reads, cleanup and retry handling without changing user-facing behaviour.
- Add regression tests for decoder recovery, split PTY reads and retry limits.
- Pin CI package sources to a Debian snapshot so removed packages do not break builds.

## 2.0.0 - 2026-08-31

- Added a C11 TNC-Pi I2C-to-KISS serial PTY bridge.
- Added systemd service management, logging, and recovery.
- Added multiple TNC instances and direct `kissattach` operation.
- Added protocol, lifecycle, and service tests.
- Added source archives and Debian packaging for native and ARM hard-float builds.
