<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$health = test_daemon_get('/health');
if ($health === null) {
    echo "document_schema_types: skipped (daemon unavailable)\n";
    exit(0);
}

$collection = 'php_schema_types_' . getmypid() . '_' . bin2hex(random_bytes(4));
$create = test_daemon_request('POST', '/collections', json_encode([
    'name' => $collection,
    'fields' => [
        ['name' => 'title', 'type' => 'string'],
        ['name' => 'counter', 'type' => 'int'],
        ['name' => 'embedding', 'type' => 'float[]'],
    ],
], JSON_THROW_ON_ERROR));
test_assert($create !== null && in_array($create['status'], [200, 201], true), 'Schema type test collection should be created');

try {
    $wrongString = test_daemon_request(
        'POST',
        "/collections/{$collection}/documents",
        '{"id":"wrong-string","title":[],"counter":1,"embedding":[1,0,0]}'
    );
    test_assert_same(400, $wrongString['status'] ?? null, 'String fields should reject arrays');

    $wrongInteger = test_daemon_request(
        'POST',
        "/collections/{$collection}/documents",
        '{"id":"wrong-integer","title":"valid","counter":"1","embedding":[1,0,0]}'
    );
    test_assert_same(400, $wrongInteger['status'] ?? null, 'Integer fields should reject numeric strings');

    $wrongVector = test_daemon_request(
        'POST',
        "/collections/{$collection}/documents",
        '{"id":"wrong-vector","title":"valid","counter":1,"embedding":[1,"zero",0]}'
    );
    test_assert_same(400, $wrongVector['status'] ?? null, 'Vector fields should reject non-numeric elements');

    $valid = test_daemon_request(
        'POST',
        "/collections/{$collection}/documents",
        '{"id":"valid","title":"valid","counter":1,"embedding":[1,0,0]}'
    );
    test_assert($valid !== null && in_array($valid['status'], [200, 201], true), 'Correctly typed document should be accepted');

    $wrongUpdate = test_daemon_request(
        'PUT',
        "/collections/{$collection}/documents/valid",
        '{"counter":[]}'
    );
    test_assert_same(400, $wrongUpdate['status'] ?? null, 'Updates should enforce declared field types');

    $wrongImport = test_daemon_request(
        'POST',
        "/collections/{$collection}/documents/import",
        '{"documents":[{"id":"bad-import","title":"valid","counter":{},"embedding":[1,0,0]}]}'
    );
    test_assert_same(200, $wrongImport['status'] ?? null, 'Bulk import should return its structured result');
    $importJson = test_assert_json_response($wrongImport, 'Wrong-type bulk import');
    test_assert_same(1, $importJson['failed'] ?? null, 'Bulk import should report the wrong-type document as failed');

    $negativeTopK = test_daemon_request(
        'POST',
        "/collections/{$collection}/vector_search",
        '{"field_name":"embedding","vector":[1,0,0],"topk":-1}'
    );
    test_assert_same(400, $negativeTopK['status'] ?? null, 'Vector search should reject a non-positive topk');
} finally {
    test_daemon_request('DELETE', "/collections/{$collection}");
}

echo "document_schema_types: ok\n";
