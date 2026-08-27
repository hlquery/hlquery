#!/bin/bash
# Auto-generated test runner

set -e

TEST_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")"; pwd)
PROJECT_DIR=$(cd "$TEST_DIR/.."; pwd)

echo "Running HLManager test suite..."
echo "Test directory: $TEST_DIR"
echo "Project directory: $PROJECT_DIR"

# Unit tests
if [ -d "$TEST_DIR/unit" ]; then
    echo "Running unit tests..."
    for test in $TEST_DIR/unit/*.cpp; do
        if [ -f "$test" ]; then
            echo "  Running $(basename $test)"
            g++ -I$PROJECT_DIR/include -I$PROJECT_DIR/src $test -o $TEST_DIR/unit_runner && $TEST_DIR/unit_runner
        fi
    done
fi

echo "Test suite completed."
