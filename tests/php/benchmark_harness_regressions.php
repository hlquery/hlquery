<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$crashPath = test_root('scripts/benchmark/crash-recovery-test.py');
$crashSource = file_get_contents($crashPath);
test_assert(is_string($crashSource) && $crashSource !== '', 'Crash-recovery verifier source should be readable');
test_assert_contains(
    'f"{collection}_{ordinal}_{run_id[2:] if len(run_id) > 2 else run_id}"',
    $crashSource,
    'Crash-recovery oracle should use the benchmark generator document ID format'
);
test_assert_contains(
    'for key in ("ordinal", "collection_number")',
    $crashSource,
    'Crash-recovery verification should normalize integer scalar fields'
);

$modesPath = test_root('src/cli/benchmarkmodes.cpp');
$modesSource = file_get_contents($modesPath);
test_assert(is_string($modesSource) && $modesSource !== '', 'Benchmark modes source should be readable');
test_assert(
    !str_contains($modesSource, '/documents/Search'),
    'Flood search should not use the case-sensitive document lookup path'
);
test_assert_contains(
    '/documents/search?q=',
    $modesSource,
    'Flood search should use the document search route'
);
test_assert_contains(
    'synonym_json["root"] = "car";',
    $modesSource,
    'Detailed synonym creation should include the required root term'
);
test_assert_contains(
    'synonym_json["synonyms"] = {"automobile", "vehicle"};',
    $modesSource,
    'Detailed synonym creation should not duplicate the root in its synonym list'
);
test_assert(
    !preg_match('/\[[0-9]+\/15\]/', $modesSource),
    'Detailed benchmark stage totals should match the implemented stages'
);
test_assert_contains('[10/10]', $modesSource, 'Detailed benchmark should report completion of ten stages');
test_assert_contains(
    'operation = OperationMetrics{};',
    $modesSource,
    'Detailed operation metrics should reset after each recorded operation'
);

$clientPath = test_root('src/cli/benchmarkclient.h');
$clientSource = file_get_contents($clientPath);
test_assert(is_string($clientSource) && $clientSource !== '', 'Benchmark client header should be readable');
foreach (['int64_t DurationMS = 0;', 'bool Success = false;', 'int ResultCount = 0;'] as $initializer) {
    test_assert_contains($initializer, $clientSource, "Operation metrics should initialize {$initializer}");
}

$mainPath = test_root('src/cli/benchmarkmain.cpp');
$mainSource = file_get_contents($mainPath);
test_assert(is_string($mainSource) && $mainSource !== '', 'Benchmark main source should be readable');
test_assert_contains(
    'return detailed_passed ? 0 : 1;',
    $mainSource,
    'Detailed benchmark failures should propagate through the process exit status'
);

echo "benchmark_harness_regressions: ok\n";
