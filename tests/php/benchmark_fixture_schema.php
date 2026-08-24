<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$fixturePath = test_root('run/benchmark/universities.json');
$fixture = test_decode_json((string) file_get_contents($fixturePath), 'Universities fixture');
$documents = $fixture['documents'] ?? null;

test_assert(is_array($documents) && $documents !== [], 'Universities fixture should contain documents');

$ids = [];
foreach ($documents as $index => $document) {
    test_assert(is_array($document), "Document {$index} should be an object");
    foreach (['id', 'title'] as $field) {
        test_assert(isset($document[$field]) && is_string($document[$field]) && trim($document[$field]) !== '', "Document {$index} should have a non-empty {$field}");
    }
    test_assert(!isset($ids[$document['id']]), "Document id {$document['id']} should be unique");
    $ids[$document['id']] = true;
}

test_assert_same(count($documents), count($ids), 'Every fixture document should have a unique id');
test_assert_same($fixture['count'] ?? null, count($documents), 'Declared fixture count should be exact');

echo "benchmark_fixture_schema: ok\n";
