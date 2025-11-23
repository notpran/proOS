#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <disk_image> <iso_output>"
    exit 1
fi

img="${1}"
iso="${2}"

img_dir=$(cd "$(dirname "${img}")" && pwd)
img_base=$(basename "${img}")
img_abs="${img_dir}/${img_base}"

iso_dir=$(cd "$(dirname "${iso}")" && pwd)
iso_base=$(basename "${iso}")
iso_abs="${iso_dir}/${iso_base}"

work_dir="${iso_dir}/iso-root"
rm -rf "${work_dir}"
mkdir -p "${work_dir}"
cp "${img_abs}" "${work_dir}/proos.img"

xorriso -as mkisofs \
    -b proos.img \
    -no-emul-boot \
    -boot-load-size 4 \
    -boot-info-table \
    -o "${iso_abs}" \
    "${work_dir}"

rm -rf "${work_dir}"
echo "ISO written to ${iso_abs}"
