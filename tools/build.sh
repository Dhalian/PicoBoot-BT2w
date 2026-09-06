#!/bin/bash
# -----------------------------------------------------------------------
# build.sh - Builds PicoBoot BT 2W firmware (Raspberry Pi Pico 2 W only)
# -----------------------------------------------------------------------
# Purpose:
#   Downloads gekkoboot, processes the DOL into a payload UF2, builds the
#   combined picoboot+bluepad32 firmware, and merges firmware + payload into
#   a single flashable UF2 in dist/.
#
# Usage:
#   ./build.sh
# -----------------------------------------------------------------------

set -e

GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

board="pico2_w"
family="rp2350-arm-s"
build_type="RelWithDebInfo"

if [ ! -f "payload.dol" ]; then
    echo -e "${RED}Error: payload.dol file not found${NC}"
    exit 1
fi

echo -e "${BLUE}##########################################################${NC}"
echo -e "🚀 ${YELLOW}Generating payload UF2 file:${NC}"
echo -e "📂 ${YELLOW}Input file:${NC} ${GREEN}payload.dol${NC}"
echo -e "${BLUE}##########################################################${NC}"

if [ ! -d "dist" ]; then
    mkdir dist
fi

python3 tools/process_ipl.py dist/payload_pico2.uf2 payload.dol rp2350

echo -e "\n🔨 ${YELLOW}Generating build files (board: ${board})...${NC}"
cmake -G Ninja -B "build/${board}" -DCMAKE_BUILD_TYPE="${build_type}" -DPICO_BOARD="${board}" -S .

echo -e "\n🔨 ${YELLOW}Building...${NC}"
cmake --build "build/${board}" --config "${build_type}"

echo -e "\n🔨 ${YELLOW}Converting firmware to a bare-metadata UF2 (family rp2350-arm-s)...${NC}"
PICOTOOL="${PICOTOOL:-build/${board}/_deps/picotool/picotool}"
if [ ! -x "${PICOTOOL}" ]; then
    command -v picotool >/dev/null 2>&1 && PICOTOOL=$(command -v picotool)
fi
"${PICOTOOL}" uf2 convert "build/${board}/dist/picoboot.bin" "build/${board}/picoboot.uf2" --family rp2350-arm-s

echo -e "\n🔨 ${YELLOW}Merging firmware + payload (via uf2tool, same tool the official PicoBoot release uses)...${NC}"
uf2tool join -o dist/picoboot_bt2w_full.uf2 "build/${board}/picoboot.uf2" "dist/payload_pico2.uf2" --family "${family}"

echo -e "\n✨ ${GREEN}Build finished! Output: dist/picoboot_bt2w_full.uf2${NC}\n"
