#!/bin/sh

set -eu

work=/tmp/ish-aarch64-paired-v1
umask 077
rm -rf "$work"
mkdir -p "$work"

printf '%s\n' \
    'alpha:0000000000000001' \
    'beta:1122334455667788' \
    'gamma:fedcba9876543210' \
    > "$work/payload"

# pipeline 的两个 applet 必须完成后，父进程才启动并行工作。
/bin/busybox cat "$work/payload" |
    /bin/busybox tr 'a-z' 'A-Z' > "$work/transformed"

/bin/busybox cat > "$work/generated.sh" <<'EOF'
#!/bin/sh
exec /bin/busybox awk 'BEGIN {
    a = 0
    b = 1
    for (i = 0; i < 20; i++) {
        sum = a + b
        a = b
        b = sum
    }
    print a
}'
EOF
chmod 0700 "$work/generated.sh"

(
    exec /bin/busybox sha256sum
) < "$work/payload" > "$work/payload.sha256" \
    2> "$work/payload.sha256.err" &
payload_pid=$!

(
    exec /bin/busybox sha256sum
) < "$work/transformed" > "$work/transformed.sha256" \
    2> "$work/transformed.sha256.err" &
transformed_pid=$!

(
    exec "$work/generated.sh"
) </dev/null > "$work/fibonacci" 2> "$work/fibonacci.err" &
fibonacci_pid=$!

/bin/busybox sh -c 'exit 7' </dev/null \
    > "$work/expected-exit.out" 2> "$work/expected-exit.err" &
expected_exit_pid=$!

set +e
wait "$payload_pid"
payload_status=$?
wait "$transformed_pid"
transformed_status=$?
wait "$fibonacci_pid"
fibonacci_status=$?
wait "$expected_exit_pid"
expected_exit_status=$?
set -e

test "$payload_status" -eq 0
test "$transformed_status" -eq 0
test "$fibonacci_status" -eq 0
test "$expected_exit_status" -eq 7
test ! -s "$work/payload.sha256.err"
test ! -s "$work/transformed.sha256.err"
test ! -s "$work/fibonacci.err"
test ! -s "$work/expected-exit.out"
test ! -s "$work/expected-exit.err"

arch=$(/bin/busybox uname -m)
payload_hash_line=$(/bin/busybox cat "$work/payload.sha256")
transformed_hash_line=$(/bin/busybox cat "$work/transformed.sha256")
fibonacci=$(/bin/busybox cat "$work/fibonacci")
generated_mode=$(/bin/busybox stat -c '%a' "$work/generated.sh")

test "$arch" = aarch64
test "$payload_hash_line" = \
    '8c7888e96cc328ebe7b14ecd9393462ad592ded3f1aea282b755b8e0a7306cd5  -'
test "$transformed_hash_line" = \
    'a4341a84b26bda06a8c0f14ad53edc539493d621f2dd10886bf9181d072e08d6  -'
test "$fibonacci" = 6765
test "$(/bin/busybox wc -l < "$work/payload.sha256")" -eq 1
test "$(/bin/busybox wc -l < "$work/transformed.sha256")" -eq 1
test "$(/bin/busybox wc -l < "$work/fibonacci")" -eq 1
test "$generated_mode" -eq 700

printf '%s\n' \
    'PAIR-V1' \
    "ARCH=$arch" \
    "PAYLOAD=${payload_hash_line%% *}" \
    "TRANSFORM=${transformed_hash_line%% *}" \
    "FIB=$fibonacci" \
    "MODE=$generated_mode" \
    "WAIT=$expected_exit_status" \
    > "$work/result"

exec /bin/busybox cat "$work/result" </dev/null
