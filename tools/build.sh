#!/bin/bash
set -e

board="pico2_w"
build_dir="rp2350_pico2_w"
family="rp2350-arm-s"
build_type="RelWithDebInfo"

if [ ! -f "payload.dol" ]; then
    echo "Error: payload.dol file not found"
    exit 1
fi

if [ ! -d "dist" ]; then
    mkdir dist
fi

python3 tools/process_ipl.py dist/payload_pico2.uf2 payload.dol rp2350

cmake -B "build/${build_dir}" -DCMAKE_BUILD_TYPE="${build_type}" -DPICO_BOARD="${board}" -S .
cmake --build "build/${build_dir}" --config "${build_type}"

picotool uf2 convert "build/${build_dir}/dist/picoboot.bin" "build/${build_dir}/picoboot.uf2" --family "${family}"

uf2tool join -o dist/picoboot_bt2w_full.uf2 "build/${build_dir}/picoboot.uf2" "dist/payload_pico2.uf2" --family "${family}"

echo "Build finished! Output: dist/picoboot_bt2w_full.uf2"
