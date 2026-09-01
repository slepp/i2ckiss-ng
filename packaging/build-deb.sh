#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Stephen Olesen

set -eu
umask 022

version=${VERSION:?VERSION is required}
binary=${BINARY:?BINARY is required}
architecture=${DEB_ARCH:?DEB_ARCH is required}
strip_tool=${STRIP:-strip}
maintainer=${MAINTAINER:-Stephen Olesen <slepp@users.noreply.github.com>}
package=i2ckiss-ng
root=build/package/${package}_${version}_${architecture}
output=dist/${package}_${version}_${architecture}.deb

case "$root" in
    build/package/*) ;;
    *) echo "unsafe package root: $root" >&2; exit 1 ;;
esac

rm -rf -- "$root"
install -d "$root/DEBIAN" "$root/usr/lib/systemd/system" \
    "$root/usr/share/doc/$package" "$root/usr/share/man/man8"
install -Dm755 "$binary" "$root/usr/sbin/i2ckiss"
"$strip_tool" --strip-unneeded "$root/usr/sbin/i2ckiss"
sed 's|@SBINDIR@|/usr/sbin|g' systemd/i2ckiss@.service.in > \
    "$root/usr/lib/systemd/system/i2ckiss@.service"
chmod 0644 "$root/usr/lib/systemd/system/i2ckiss@.service"
gzip -n -9 -c docs/i2ckiss.8 > "$root/usr/share/man/man8/i2ckiss.8.gz"
install -Dm644 LICENSE "$root/usr/share/doc/$package/copyright"
install -Dm644 README.md -t "$root/usr/share/doc/$package"
install -Dm644 docs/device-aliases.md docs/migration.md docs/multiple-tncs.md \
    -t "$root/usr/share/doc/$package/docs"
gzip -n -9 -c CHANGELOG.md > "$root/usr/share/doc/$package/changelog.gz"
install -Dm644 systemd/i2ckiss.example.conf "$root/etc/i2ckiss/example.conf"
install -Dm644 systemd/i2ckiss-tmpfiles.example.conf \
    "$root/usr/share/doc/$package/examples/i2ckiss-tmpfiles.conf"

installed_size=$(du -sk "$root/usr" | awk '{print $1}')
libc_min=$(
    LC_ALL=C readelf --version-info "$root/usr/sbin/i2ckiss" |
        sed -n 's/.*Name: GLIBC_\([0-9][0-9.]*\).*/\1/p' |
        sort -V |
        tail -n 1
)
test -n "$libc_min"
cat > "$root/DEBIAN/control" <<EOF
Package: $package
Version: $version
Section: hamradio
Priority: optional
Architecture: $architecture
Maintainer: $maintainer
Installed-Size: $installed_size
Depends: libc6 (>= $libc_min), systemd
Suggests: ax25-tools
Homepage: https://github.com/slepp/i2ckiss-ng
Description: reliable I2C-to-KISS bridge for TNC-Pi hardware
 i2ckiss-ng presents a TNC-Pi or TNC-Black I2C modem as a KISS serial
 pseudo-terminal and provides systemd service management.
EOF

cat > "$root/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi
EOF

cat > "$root/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload || true
fi
EOF

chmod 0755 "$root/DEBIAN/postinst" "$root/DEBIAN/postrm"
find "$root" -type d -exec chmod 0755 {} +
dpkg-deb --root-owner-group --build "$root" "$output"
