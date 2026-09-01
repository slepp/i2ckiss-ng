#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Stephen Olesen

set -eu
umask 022

output=${1:?usage: build-source.sh output.tar.gz}
version=$(sed -n '1p' VERSION)
package=i2ckiss-ng-$version
epoch=${SOURCE_DATE_EPOCH:-1788134400}

tar --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$epoch" --transform="s,^,$package/," \
    --verbatim-files-from --files-from=packaging/source-files.txt -cf - |
    gzip -n -9 > "$output"
