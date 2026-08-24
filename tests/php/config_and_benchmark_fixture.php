<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$fixturePath = test_root('run/benchmark/universities.json');

test_assert(is_file($fixturePath), 'Expected run/benchmark/universities.json to exist');

$configPaths = [
    test_root('run/conf/hlquery.conf'),
    test_root('2nd/run/conf/hlquery.conf'),
    test_root('3rd/run/conf/hlquery.conf'),
];
foreach ($configPaths as $configPath) {
    test_assert(is_file($configPath), "Expected {$configPath} to exist");
    $config = file_get_contents($configPath);
    test_assert(is_string($config) && $config !== '', "Expected {$configPath} to be readable");
    test_assert(strpos($config, 'wal_sync_mode="normal"') !== false, "Expected {$configPath} to use wal_sync_mode=normal");
}

$fixture = json_decode((string) file_get_contents($fixturePath), true);
test_assert(is_array($fixture), 'Universities benchmark fixture should decode as JSON');
test_assert(($fixture['collection'] ?? null) === 'universities', 'Universities fixture should declare the universities collection');
test_assert(($fixture['count'] ?? null) === 100, 'Universities fixture should contain 100 documents');
test_assert(isset($fixture['documents']) && is_array($fixture['documents']), 'Universities fixture should contain documents');
test_assert(count($fixture['documents']) === $fixture['count'], 'Universities document count should match its declared count');

$titles = array_values(array_filter(array_map(static function ($doc) {
    return is_array($doc) ? ($doc['title'] ?? null) : null;
}, $fixture['documents'])));

test_assert(in_array('University of Chicago', $titles, true), 'Universities fixture should include University of Chicago');
test_assert(in_array('Harvard University', $titles, true), 'Universities fixture should include Harvard University');

echo "config_and_benchmark_fixture: ok\n";
