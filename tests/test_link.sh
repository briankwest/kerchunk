#!/bin/sh
# Integration test for kerchunk-reflectd ↔ kerchunk-link-probe.
#
# Run via `make check`. Builds nothing — relies on the binaries from the
# top builddir. Each "case" is one shell command; if any fails the whole
# script fails, taking the test suite with it.
#
# Coverage grows phase by phase:
#   Phase 1+2: control plane (login, ping, set_tg, floor)
#   Phase 3+:  RTP/SRTP audio round-trip (added with audio cases)

set -eu

# Locate binaries: builddir takes precedence over PATH so an in-tree
# build is always exercised over any system install.
BUILDDIR="${top_builddir:-.}"
SRCDIR="${top_srcdir:-.}"
REFL="$BUILDDIR/kerchunk-reflectd"
PROBE="$BUILDDIR/kerchunk-link-probe"

if [ ! -x "$REFL" ] || [ ! -x "$PROBE" ]; then
    echo "test_link: binaries not found at $REFL / $PROBE" >&2
    exit 77   # skip — autotools convention
fi

# Pick a port unlikely to collide with anything else; can be overridden.
PORT="${TEST_LINK_PORT:-19443}"
URL="ws://127.0.0.1:$PORT/link"

# Workdir + cleanup
WORK=$(mktemp -d)
CONF="$WORK/reflectd.conf"
LOG="$WORK/reflectd.log"
REFL_PID=""
cleanup() {
    [ -n "${REFL_PID:-}" ] && kill -INT "$REFL_PID" 2>/dev/null || true
    [ -n "${REFL_PID:-}" ] && wait "$REFL_PID" 2>/dev/null || true
    # Belt + suspenders in case INT was missed.
    pkill -KILL -P $$ 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

ALPHA_PSK=0101010101010101010101010101010101010101010101010101010101010101
BETA_PSK=0202020202020202020202020202020202020202020202020202020202020202
GAMMA_PSK=0303030303030303030303030303030303030303030303030303030303030303
WRONG_PSK=0000000000000000000000000000000000000000000000000000000000000000

cat >"$CONF" <<EOF
[reflector]
listen_url = ws://127.0.0.1:$PORT/link
keepalive_s = 15
hangtime_ms = 1500

[node.alpha]
preshared_key_hex = $ALPHA_PSK
allowed_tgs = 100, 101
default_tg = 100

[node.beta]
preshared_key_hex = $BETA_PSK
allowed_tgs = 100, 101
default_tg = 100

[node.gamma]
preshared_key_hex = $GAMMA_PSK
allowed_tgs = 101
default_tg = 101

[talkgroup.100]
name = "T100"
nodes = alpha, beta

[talkgroup.101]
name = "T101"
nodes = alpha, beta, gamma
EOF

# Boot reflectd. Fail fast if it doesn't come up.
"$REFL" -c "$CONF" >"$LOG" 2>&1 &
REFL_PID=$!
# Poll for the listener instead of sleeping a fixed amount.
i=0
while [ $i -lt 20 ]; do
    if ss -tln 2>/dev/null | grep -q ":$PORT "; then break; fi
    sleep 0.1
    i=$((i + 1))
done
if [ $i -ge 20 ]; then
    echo "test_link: reflectd didn't listen on $PORT in 2s" >&2
    cat "$LOG" >&2
    exit 1
fi

# ── Helpers ────────────────────────────────────────────────────────

pass() { printf "  PASS  %s\n" "$1"; }
fail() { printf "  FAIL  %s\n" "$1" >&2; cat "$LOG" >&2; exit 1; }

expect_ok() {
    local label="$1"; shift
    if "$PROBE" "$@" -u "$URL" -T 5 >"$WORK/out" 2>"$WORK/err"; then
        pass "$label"
    else
        rc=$?
        fail "$label  rc=$rc  stdout=$(tr '\n' '|' <"$WORK/out")  stderr=$(tr '\n' '|' <"$WORK/err")"
    fi
}

expect_fail() {
    local label="$1"; shift
    if "$PROBE" "$@" -u "$URL" -T 5 >"$WORK/out" 2>"$WORK/err"; then
        fail "$label  unexpectedly succeeded  stdout=$(tr '\n' '|' <"$WORK/out")"
    else
        pass "$label"
    fi
}

expect_in_stdout() {
    local label="$1" needle="$2"; shift 2
    "$PROBE" "$@" -u "$URL" -T 5 >"$WORK/out" 2>"$WORK/err" || true
    if grep -q "$needle" "$WORK/out"; then
        pass "$label"
    else
        fail "$label  missing '$needle'  stdout=$(tr '\n' '|' <"$WORK/out")"
    fi
}

# ── Phase 1+2 cases ────────────────────────────────────────────────

echo "Phase 1+2 control-plane tests:"

expect_ok   "happy login + 3 ping/pong"     -n alpha -k "$ALPHA_PSK" -c 3
expect_fail "login_denied(bad_key)"          -n alpha -k "$WRONG_PSK"  -c 1
expect_fail "login_denied(unknown_node)"     -n delta -k "$ALPHA_PSK"  -c 1
expect_ok   "set_tg allowed"                 -n alpha -k "$ALPHA_PSK"  --tg 101 -c 0
expect_in_stdout "set_tg unknown → tg_denied(unknown_tg)" \
    "code=unknown_tg"                        -n alpha -k "$ALPHA_PSK"  --tg 9999 -c 0
expect_in_stdout "set_tg unauth → tg_denied(not_authorized)" \
    "code=not_authorized"                    -n gamma -k "$GAMMA_PSK"  --tg 100 -c 0
expect_in_stdout "solo talker echoes own talker(node_id)" \
    "talker: tg=100 node=alpha"              -n alpha -k "$ALPHA_PSK"  --talk

# Floor contention: alpha holds the floor, beta tries to talk → floor_denied.
echo "Phase 2 multi-client floor contention:"
"$PROBE" -n alpha -k "$ALPHA_PSK" -u "$URL" --hold-ms 1500 -T 5 \
    >"$WORK/alpha.out" 2>&1 &
ALPHA_PID=$!
sleep 0.4
"$PROBE" -n beta -k "$BETA_PSK" -u "$URL" --talk -T 3 \
    >"$WORK/beta.out" 2>&1 &
BETA_PID=$!
wait $BETA_PID || true
wait $ALPHA_PID || true
if grep -q "floor_denied: current_talker=alpha" "$WORK/beta.out"; then
    pass "beta sees floor_denied(current_talker=alpha) while alpha holds"
else
    fail "beta did not see floor_denied — got: $(tr '\n' '|' <"$WORK/beta.out")"
fi

# Same node connecting twice: second login → node_busy. First probe must
# stay connected while the second one tries; --hold-ms 3000 + the periodic
# refresh keep alpha's session live well beyond the second probe's lifetime.
echo "Phase 2 node_busy enforcement:"
"$PROBE" -n alpha -k "$ALPHA_PSK" -u "$URL" --hold-ms 3000 -T 6 \
    >"$WORK/a1.out" 2>"$WORK/a1.err" &
ALPHA_PID=$!
sleep 0.5
"$PROBE" -n alpha -k "$ALPHA_PSK" -u "$URL" -c 1 -T 3 \
    >"$WORK/a2.out" 2>"$WORK/a2.err" || true
if grep -q "login_denied(node_busy)" "$WORK/a2.err"; then
    pass "second 'alpha' login_denied(node_busy)"
else
    fail "did not see node_busy — a2.err: $(tr '\n' '|' <"$WORK/a2.err") a1.err: $(tr '\n' '|' <"$WORK/a1.err")"
fi
wait $ALPHA_PID || true

echo "test_link: all cases passed"
exit 0
