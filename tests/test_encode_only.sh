#!/bin/bash
# Drives tests/test_encode_only against the CUDA-less encode-only path.
#
# CUDA_VISIBLE_DEVICES="" makes cuInit() fail with "no CUDA-capable device",
# which is exactly what a 32-bit client sees on a Blackwell card and what puts
# the driver into IPC encode-only mode on a 64-bit box.
#
# Args: $1 = test_encode_only binary, $2 = nvenc-helper binary
set -u

TEST_BIN=${1:?usage: test_encode_only.sh <test-binary> <nvenc-helper>}
HELPER_BIN=${2:?usage: test_encode_only.sh <test-binary> <nvenc-helper>}

export CUDA_VISIBLE_DEVICES=""
export LIBVA_DRIVER_NAME=nvidia

# A private runtime dir keeps the helper this test starts off the user's real
# socket -- and keeps a helper the user is already running out of the test.
# Kept short: it ends up in sockaddr_un.sun_path, which is 108 bytes.
RUNTIME_DIR=$(mktemp -d /tmp/nvd-encode-only.XXXXXX)
chmod 700 "$RUNTIME_DIR"
HELPER_PID=""
cleanup() {
    if [ -n "$HELPER_PID" ]; then
        kill "$HELPER_PID" 2>/dev/null
        wait "$HELPER_PID" 2>/dev/null
    fi
    rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT

RC=0

# --- No helper reachable ---
# Nothing is listening on this socket and NVD_NVENC_HELPER points at nothing
# executable, so the capability query must fail. The driver still has to
# advertise its built-in encode profiles: answering "no encode entrypoints"
# here is what silently dropped Steam to software x264.
XDG_RUNTIME_DIR="$RUNTIME_DIR" NVD_NVENC_HELPER="$RUNTIME_DIR/no-such-helper" \
    "$TEST_BIN" fallback
STATUS=$?
if [ $STATUS -eq 77 ]; then
    echo "encode-only mode not reachable on this machine"
    exit 77
fi
[ $STATUS -ne 0 ] && RC=1

# --- Helper reachable ---
# Capabilities now come from the helper's GPU, and a real frame goes through it.
# The helper keeps its CUDA: in the real deployment only the client process is
# the one that cannot reach the GPU. Unset rather than override so it sees the
# machine's default device selection.
env -u CUDA_VISIBLE_DEVICES XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    "$HELPER_BIN" --foreground >"$RUNTIME_DIR/helper.log" 2>&1 &
HELPER_PID=$!
for _ in $(seq 1 50); do
    [ -S "$RUNTIME_DIR/nvenc-helper.sock" ] && break
    sleep 0.1
done
if [ ! -S "$RUNTIME_DIR/nvenc-helper.sock" ]; then
    echo "nvenc-helper failed to start:"
    cat "$RUNTIME_DIR/helper.log"
    exit 1
fi

XDG_RUNTIME_DIR="$RUNTIME_DIR" NVD_NVENC_HELPER="$HELPER_BIN" "$TEST_BIN" helper || RC=1

if ! grep -q "^\[nvenc-helper.*\] Caps:" "$RUNTIME_DIR/helper.log"; then
    echo "helper never served a capability query -- the driver did not ask:"
    cat "$RUNTIME_DIR/helper.log"
    RC=1
fi

exit $RC
