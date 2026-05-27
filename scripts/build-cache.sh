#!/usr/bin/env bash

set -euo pipefail

bindir=$1
datadir=$2

prefix=${MESON_INSTALL_DESTDIR_PREFIX:-${MESON_INSTALL_PREFIX:-}}
if [[ -z "$prefix" ]]; then
	echo "MESON_INSTALL_DESTDIR_PREFIX is not set" >&2
	exit 1
fi

if [[ "$bindir" = /* ]]; then
	bin_path="$bindir/gd-frequency"
else
	bin_path="$prefix/$bindir/gd-frequency"
fi

if [[ "$datadir" = /* ]]; then
	share_dir="$datadir/gd-frequency"
else
	share_dir="$prefix/$datadir/gd-frequency"
fi

"$bin_path" --build-cache \
	--dict-path "$share_dir/JPDB_v2.2_Frequency_2024-10-13.zip" \
	--dict-path "$share_dir/H_Freq.zip" \
	--dict-path "$share_dir/vn_freq.zip" \
	--bin-path "$share_dir/dict.bin"
