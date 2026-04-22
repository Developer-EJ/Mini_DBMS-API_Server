#!/usr/bin/env bash

set -u

: "${LOCK_PORT:=18080}"
: "${NOLOCK_PORT:=18081}"
: "${TOTAL:=2000}"
: "${BATCH:=50}"
: "${WORKERS:=2}"

BASE_DIR="$(cd "$(dirname "$0")" && pwd)"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mini-dbms-atomicity.XXXXXX")"
LOCK_PID=""
NOLOCK_PID=""

cleanup() {
    if [ -n "$LOCK_PID" ]; then
        kill "$LOCK_PID" 2>/dev/null || true
        wait "$LOCK_PID" 2>/dev/null || true
    fi
    if [ -n "$NOLOCK_PID" ]; then
        kill "$NOLOCK_PID" 2>/dev/null || true
        wait "$NOLOCK_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

require_binary() {
    local path=$1
    if [ ! -x "$path" ]; then
        echo "필요한 실행 파일이 없습니다: $path"
        echo "먼저 실행하세요: make sqlpd sqlpd_nolock"
        exit 1
    fi
}

wait_for_server() {
    local port=$1
    local name=$2

    for _ in $(seq 1 40); do
        if curl -s --max-time 1 "http://localhost:$port/not-found" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done

    echo "$name 서버가 port $port 에서 시작되지 않았습니다."
    exit 1
}

count_lines() {
    local file=$1
    if [ -f "$file" ]; then
        wc -l < "$file" | tr -d ' '
    else
        echo 0
    fi
}

count_unique_ids() {
    local file=$1
    if [ -f "$file" ]; then
        awk -F ' *\\| *' 'NF >= 1 && $1 != "" { seen[$1] = 1 }
            END { for (id in seen) n++; print n + 0 }' "$file"
    else
        echo 0
    fi
}

extract_selected_id() {
    awk -F '|' '
        function trim(s) {
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", s)
            return s
        }
        /^\|/ {
            id = trim($2)
            if (id != "" && id != "id") {
                print id
                exit
            }
        }
    '
}

INSERT_BAD_HTTP=0
INSERT_BAD_JSON=0

echo "========================================"
echo "  원자성 테스트 (INSERT $TOTAL 건)"
echo "  락 있음  → port $LOCK_PORT"
echo "  락 없음  → port $NOLOCK_PORT"
echo "========================================"

rm -f "$BASE_DIR/data/users_lock.dat"
rm -f "$BASE_DIR/data/users_nolock.dat"

cd "$BASE_DIR"
require_binary "$BASE_DIR/sqlpd"
require_binary "$BASE_DIR/sqlpd_nolock"

./sqlpd        "$LOCK_PORT"   "$WORKERS" 4096 > "$TMP_DIR/sqlpd-lock.log" 2>&1 &
LOCK_PID=$!
./sqlpd_nolock "$NOLOCK_PORT" "$WORKERS" 4096 > "$TMP_DIR/sqlpd-nolock.log" 2>&1 &
NOLOCK_PID=$!

wait_for_server "$LOCK_PORT" "락 있음"
wait_for_server "$NOLOCK_PORT" "락 없음"

send_batch() {
    local port=$1
    local table=$2
    local from=$3
    local to=$4
    local pids=()
    local code_files=()
    local body_files=()
    local i

    for i in $(seq "$from" "$to"); do
        local code_file="$TMP_DIR/${table}_${i}.code"
        local body_file="$TMP_DIR/${table}_${i}.body"
        curl -s --max-time 5 -o "$body_file" -w "%{http_code}" \
            -X POST "http://localhost:$port/sql" \
            -d "sql=INSERT INTO $table VALUES ($i, 'user$i', $((20 + i % 50)), 'u$i@test.com')" \
            > "$code_file" &
        pids+=("$!")
        code_files+=("$code_file")
        body_files+=("$body_file")
    done

    for i in "${!pids[@]}"; do
        wait "${pids[$i]}" || true

        if [ "$(cat "${code_files[$i]}" 2>/dev/null)" != "200" ]; then
            INSERT_BAD_HTTP=$((INSERT_BAD_HTTP + 1))
        fi
        if ! grep -q '"ok":true' "${body_files[$i]}" 2>/dev/null; then
            INSERT_BAD_JSON=$((INSERT_BAD_JSON + 1))
        fi
    done
}

run_inserts() {
    local port=$1
    local table=$2
    local label=$3
    local batch_start
    local batch_end

    INSERT_BAD_HTTP=0
    INSERT_BAD_JSON=0

    echo ""
    echo "$label INSERT $TOTAL 건..."
    for batch_start in $(seq 1 "$BATCH" "$TOTAL"); do
        batch_end=$((batch_start + BATCH - 1))
        [ "$batch_end" -gt "$TOTAL" ] && batch_end=$TOTAL
        send_batch "$port" "$table" "$batch_start" "$batch_end"
        printf "  진행: %d / %d\r" "$batch_end" "$TOTAL"
    done
    echo ""
    echo "  완료"
}

verify_selects() {
    local port=$1
    local table=$2
    local mismatch=0
    local i
    local body
    local actual_id

    for i in $(seq 1 "$TOTAL"); do
        body=$(curl -s --max-time 5 --get \
            --data-urlencode "sql=SELECT * FROM $table WHERE id = $i;" \
            "http://localhost:$port/sql" || true)
        actual_id=$(printf "%s" "$body" | extract_selected_id)
        if [ "$actual_id" != "$i" ]; then
            mismatch=$((mismatch + 1))
        fi
    done

    echo "$mismatch"
}

run_inserts "$NOLOCK_PORT" "users_nolock" "[1/2] 락 없는 서버"
NOLOCK_BAD_HTTP=$INSERT_BAD_HTTP
NOLOCK_BAD_JSON=$INSERT_BAD_JSON

echo ""
echo "[검증] 락 없는 서버 SELECT offset 일치 확인..."
NOLOCK_MISMATCH=$(verify_selects "$NOLOCK_PORT" "users_nolock")

run_inserts "$LOCK_PORT" "users_lock" "[2/2] 락 있는 서버"
LOCK_BAD_HTTP=$INSERT_BAD_HTTP
LOCK_BAD_JSON=$INSERT_BAD_JSON

echo ""
echo "[검증] 락 있는 서버 SELECT offset 일치 확인..."
LOCK_MISMATCH=$(verify_selects "$LOCK_PORT" "users_lock")

NOLOCK_COUNT=$(count_lines "$BASE_DIR/data/users_nolock.dat")
LOCK_COUNT=$(count_lines "$BASE_DIR/data/users_lock.dat")
NOLOCK_UNIQUE=$(count_unique_ids "$BASE_DIR/data/users_nolock.dat")
LOCK_UNIQUE=$(count_unique_ids "$BASE_DIR/data/users_lock.dat")

cleanup
trap - EXIT INT TERM

echo ""
echo "========================================"
echo "  결과"
echo "----------------------------------------"
printf "  %-8s 요청=%5d  라인=%5d  고유 id=%5d  HTTP 실패=%3d  JSON 실패=%3d  SELECT 불일치=%3d\n" \
    "락 없음" "$TOTAL" "$NOLOCK_COUNT" "$NOLOCK_UNIQUE" "$NOLOCK_BAD_HTTP" "$NOLOCK_BAD_JSON" "$NOLOCK_MISMATCH"
printf "  %-8s 요청=%5d  라인=%5d  고유 id=%5d  HTTP 실패=%3d  JSON 실패=%3d  SELECT 불일치=%3d\n" \
    "락 있음" "$TOTAL" "$LOCK_COUNT" "$LOCK_UNIQUE" "$LOCK_BAD_HTTP" "$LOCK_BAD_JSON" "$LOCK_MISMATCH"
echo "========================================"
