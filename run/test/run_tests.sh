#!/usr/bin/env bash

set -euo pipefail

TEST_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")"; pwd)
PROJECT_DIR=$(cd "$TEST_DIR/../.."; pwd)

SERVER_BIN=${HLQUERY_SERVER_BIN:-"$PROJECT_DIR/build/bin/hlquery"}
BASE_URL=${HLQUERY_BASE_URL:-"http://127.0.0.1:19200"}
SOURCE_CONFIG=${HLQUERY_CONFIG:-"$PROJECT_DIR/run/conf/hlquery.conf"}
KEEP_DATA=${HLQUERY_KEEP_DATA:-0}
RUN_PHP_TESTS=${HLQUERY_RUN_PHP_TESTS:-1}
RUN_JS_TESTS=${HLQUERY_RUN_JS_TESTS:-1}
RUN_SQL_TESTS=${HLQUERY_RUN_SQL_TESTS:-0}
RUN_SLOW_TESTS=${HLQUERY_RUN_SLOW_TESTS:-0}

TMP_ROOT=""
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi

    if [ -n "$TMP_ROOT" ] && [ "$KEEP_DATA" != "1" ]; then
        rm -rf "$TMP_ROOT"
    elif [ -n "$TMP_ROOT" ]; then
        echo "[test] kept temporary data at $TMP_ROOT"
    fi
}

trap cleanup EXIT INT TERM

require_file() {
    if [ ! -f "$1" ]; then
        echo "[test] missing required file: $1" >&2
        exit 1
    fi
}

wait_for_server() {
    local deadline=$((SECONDS + 30))

    while [ "$SECONDS" -lt "$deadline" ]; do
        if command -v curl >/dev/null 2>&1; then
            if curl -fsS "$BASE_URL/health" >/dev/null 2>&1; then
                return 0
            fi
        else
            python3 - "$BASE_URL" >/dev/null 2>&1 <<'PY' && return 0 || true
import sys
from urllib.request import urlopen
urlopen(sys.argv[1] + "/health", timeout=1).read()
PY
        fi

        sleep 0.25
    done

    echo "[test] server did not become healthy at $BASE_URL" >&2
    exit 1
}

build_temp_config() {
    TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/hlquery-tests.XXXXXX")
    mkdir -p "$TMP_ROOT/data"

    local config_dir
    config_dir=$(dirname "$SOURCE_CONFIG")

    find "$config_dir" -maxdepth 1 -type f -name '*.conf' ! -name "$(basename "$SOURCE_CONFIG")" \
        -exec cp {} "$TMP_ROOT/" \;

    local port
    port=$(python3 - "$BASE_URL" <<'PY'
import sys
from urllib.parse import urlparse
url = urlparse(sys.argv[1])
print(url.port or (443 if url.scheme == "https" else 80))
PY
)

    python3 - "$SOURCE_CONFIG" "$TMP_ROOT/hlquery.conf" "$port" <<'PY'
import re
import sys
src, dst, port = sys.argv[1:4]
data = open(src, encoding="utf-8").read()
data = re.sub(r'<server name="[^"]*" id="[^"]*">',
              '<server name="hlquery.test" id="test">', data, count=1)
data = re.sub(r'<bind address="[^"]*" port="[0-9]+" type="http">',
              f'<bind address="127.0.0.1" port="{port}" type="http">', data, count=1)
open(dst, "w", encoding="utf-8").write(data)
PY
}

run_unit_tests() {
    if [ ! -d "$TEST_DIR/unit" ]; then
        return 0
    fi

    echo "[test] running C++ unit tests"
    shopt -s nullglob
    local tests=("$TEST_DIR"/unit/*.cpp)
    shopt -u nullglob

    for test in "${tests[@]}"; do
        echo "[test] unit $(basename "$test")"
        g++ -std=c++20 -I"$PROJECT_DIR/include" -I"$PROJECT_DIR/src" "$test" -o "$TMP_ROOT/unit_runner"
        "$TMP_ROOT/unit_runner"
    done
}

run_php_http_tests() {
    if [ "$RUN_PHP_TESTS" != "1" ]; then
        return 0
    fi

    if ! command -v php >/dev/null 2>&1; then
        echo "[test] php not found; skipping PHP HTTP tests"
        return 0
    fi

    require_file "$SERVER_BIN"
    require_file "$TMP_ROOT/hlquery.conf"

    echo "[test] starting managed server for PHP HTTP tests"
    HLQUERY_DATA_DIR="$TMP_ROOT/data" "$SERVER_BIN" --nofork --nopid --skip-auth --config "$TMP_ROOT/hlquery.conf" &
    SERVER_PID=$!
    wait_for_server

    local php_tests=(
        "$PROJECT_DIR/tests/search_syntax.php"
        "$PROJECT_DIR/tests/maybe_scope.php"
        "$PROJECT_DIR/tests/university.php"
        "$PROJECT_DIR/tests/fake.php"
    )

    if [ "$RUN_SQL_TESTS" = "1" ]; then
        php_tests+=("$PROJECT_DIR/tests/sql.php")
    fi

    for test in "${php_tests[@]}"; do
        if [ -f "$test" ]; then
            echo "[test] php $(basename "$test")"
            php "$test" "$BASE_URL"
        fi
    done

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

run_js_http_tests() {
    if [ "$RUN_JS_TESTS" != "1" ]; then
        return 0
    fi

    if ! command -v node >/dev/null 2>&1; then
        echo "[test] node not found; skipping JS HTTP tests"
        return 0
    fi

    require_file "$SERVER_BIN"

    if [ -f "$PROJECT_DIR/tests/benchmark_restart_consistency.js" ]; then
        echo "[test] js benchmark_restart_consistency.js"
        HLQUERY_BASE_URL="$BASE_URL" \
        HLQUERY_SERVER_BIN="$SERVER_BIN" \
        HLQUERY_CONFIG="$SOURCE_CONFIG" \
        HLQUERY_COLLECTIONS="${HLQUERY_COLLECTIONS:-2}" \
        HLQUERY_DOCUMENTS="${HLQUERY_DOCUMENTS:-200}" \
        node "$PROJECT_DIR/tests/benchmark_restart_consistency.js"
    fi

    if [ "$RUN_SLOW_TESTS" = "1" ] && [ -f "$PROJECT_DIR/tests/lazy_index_first_search.js" ]; then
        echo "[test] js lazy_index_first_search.js"
        HLQUERY_BASE_URL="$BASE_URL" \
        HLQUERY_SERVER_BIN="$SERVER_BIN" \
        HLQUERY_CONFIG="$SOURCE_CONFIG" \
        HLQUERY_DOCUMENTS="${HLQUERY_DOCUMENTS:-12000}" \
        node "$PROJECT_DIR/tests/lazy_index_first_search.js"
    fi
}

echo "[test] hlquery test suite"
echo "[test] project: $PROJECT_DIR"

require_file "$SOURCE_CONFIG"
build_temp_config
run_unit_tests
run_php_http_tests
run_js_http_tests

echo "[test] test suite completed"
