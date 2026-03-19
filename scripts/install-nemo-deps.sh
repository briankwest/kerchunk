#!/bin/bash
# install-nemo-deps.sh — Install OpenFst and build libnemo_normalize
#
# Only requires OpenFst (no Thrax dependency).
# Run as: sudo ./scripts/install-nemo-deps.sh
# Or:     ./scripts/install-nemo-deps.sh  (will prompt for sudo)

set -e

echo "=== Installing OpenFst + building libnemo_normalize ==="
echo ""

# ── Step 1: Install OpenFst from Ubuntu packages ──
echo ">>> Step 1/3: Installing OpenFst (apt)..."
sudo apt-get update -qq
sudo apt-get install -y libfst-dev libfst26 libfst26-plugins-base
echo "    OpenFst installed."
echo ""

# ── Step 2: Build libnemo_normalize ──
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERCHUNK_DIR="$(dirname "$SCRIPT_DIR")"
NEMO_DIR="$KERCHUNK_DIR/libnemo_normalize"

if [ -f "$NEMO_DIR/Makefile" ]; then
    echo ">>> Step 2/3: Building libnemo_normalize..."
    make -C "$NEMO_DIR" CONDA_PREFIX=/usr clean 2>/dev/null || true
    make -C "$NEMO_DIR" CONDA_PREFIX=/usr
    echo "    libnemo_normalize.so built."
    echo ""
else
    echo ">>> Step 2/3: libnemo_normalize submodule not found."
    echo "    Run: git submodule update --init"
    exit 1
fi

# ── Step 3: Verify ──
echo ">>> Step 3/3: Verification..."
echo -n "  libfst: "
ldconfig -p 2>/dev/null | grep -q 'libfst\.so' && echo "OK" || echo "MISSING"
echo -n "  libfstfar: "
ldconfig -p 2>/dev/null | grep -q 'libfstfar' && echo "OK" || echo "MISSING"
if [ -f "$NEMO_DIR/libnemo_normalize.so" ]; then
    echo "  libnemo_normalize.so: OK"
else
    echo "  libnemo_normalize.so: NOT BUILT"
    exit 1
fi

# Quick test if available
if [ -f "$NEMO_DIR/test_normalize" ]; then
    echo ""
    echo "  Running quick test..."
    LD_LIBRARY_PATH="$NEMO_DIR:/usr/lib" "$NEMO_DIR/test_normalize" 2>/dev/null && echo "  Test: PASS" || echo "  Test: FAIL (FAR files may need regeneration)"
fi

echo ""
echo "=== Next steps ==="
echo "  1. Rebuild kerchunkd:  make clean && make all"
echo "  2. Add to kerchunk.conf [tts] section:"
echo "     normalize_far_dir = $NEMO_DIR/far_export"
echo "  3. Clear TTS cache:    ./kerchunk -x 'tts cache-clear'"
echo "  4. Restart kerchunkd"
echo ""
echo "Done."
