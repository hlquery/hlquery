<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$replicaUrl = getenv('HLQUERY_REPLICA_URL');
if (!is_string($replicaUrl) || $replicaUrl === '') {
    echo "replica_readonly: skipped (set HLQUERY_REPLICA_URL)\n";
    exit(0);
}

$primaryUrl = getenv('HLQUERY_URL') ?: 'http://127.0.0.1:9200';
putenv('HLQUERY_URL=' . $replicaUrl);

$health = test_daemon_get('/health');
test_assert($health !== null, 'Replica health endpoint should be reachable');
test_assert_same(200, $health['status'], 'Healthy replica should return HTTP 200');
$healthJson = test_assert_json_response($health, 'Replica health endpoint');
test_assert_same(true, $healthJson['readonly_mode'] ?? null, 'Replica should advertise read-only mode');

$response = test_daemon_request(
    'POST',
    '/collections/__replica_readonly_probe__/documents?distributed=off',
    '{"id":"must-not-be-created","title":"blocked"}'
);
test_assert($response !== null, 'Replica write attempt should receive a response');
test_assert_same(403, $response['status'], 'A policy-blocked replica write should return HTTP 403');
$json = test_assert_json_response($response, 'Replica read-only response');
test_assert_same('Read-only replica', $json['error'] ?? null, 'Replica should return its read-only diagnostic');

$collection = 'php_replication_' . getmypid() . '_' . bin2hex(random_bytes(4));
putenv('HLQUERY_URL=' . $primaryUrl);
$create = test_daemon_request('POST', '/collections', json_encode([
    'name' => $collection,
    'fields' => [
        ['name' => 'title', 'type' => 'string'],
        ['name' => 'content', 'type' => 'string'],
        ['name' => 'embedding', 'type' => 'float[]'],
    ],
], JSON_THROW_ON_ERROR));
test_assert($create !== null, 'Primary collection creation should receive a response');
test_assert(in_array($create['status'], [200, 201], true), 'Primary should create the replication test collection');

$add = test_daemon_request(
    'POST',
    "/collections/{$collection}/documents",
    '{"title":"generated identity","content":"metadata must converge","embedding":[1,0,0]}'
);
test_assert($add !== null, 'Primary auto-ID insert should receive a response');
test_assert(in_array($add['status'], [200, 201], true), 'Primary auto-ID insert should succeed');
$addJson = test_assert_json_response($add, 'Primary auto-ID insert');
$documentId = $addJson['id'] ?? null;
test_assert(is_string($documentId) && $documentId !== '', 'Primary should return its generated document ID');

$primaryDocument = test_daemon_get("/collections/{$collection}/documents/{$documentId}?distributed=off");
test_assert($primaryDocument !== null && $primaryDocument['status'] === 200, 'Primary generated document should be readable');
$primaryJson = test_assert_json_response($primaryDocument, 'Primary generated document');

putenv('HLQUERY_URL=' . $replicaUrl);
$replicaDocument = test_daemon_get("/collections/{$collection}/documents/{$documentId}?distributed=off");
test_assert($replicaDocument !== null && $replicaDocument['status'] === 200, 'Replica should receive the generated document ID');
$replicaJson = test_assert_json_response($replicaDocument, 'Replica generated document');
test_assert_same($primaryJson['timestamp'] ?? null, $replicaJson['timestamp'] ?? null, 'Replicated document timestamp should match primary');
test_assert_same($primaryJson['created_at'] ?? null, $replicaJson['created_at'] ?? null, 'Replicated document creation time should match primary');

putenv('HLQUERY_URL=' . $primaryUrl);
$update = test_daemon_request(
    'PUT',
    "/collections/{$collection}/documents/{$documentId}",
    '{"title":"updated identity","content":"updated metadata must converge","embedding":[0,1,0]}'
);
test_assert($update !== null && $update['status'] === 200, 'Primary replicated update should succeed');
$primaryUpdated = test_daemon_get("/collections/{$collection}/documents/{$documentId}?distributed=off");
test_assert($primaryUpdated !== null && $primaryUpdated['status'] === 200, 'Updated primary document should be readable');
$primaryUpdatedJson = test_assert_json_response($primaryUpdated, 'Updated primary document');

putenv('HLQUERY_URL=' . $replicaUrl);
$replicaUpdated = test_daemon_get("/collections/{$collection}/documents/{$documentId}?distributed=off");
test_assert($replicaUpdated !== null && $replicaUpdated['status'] === 200, 'Updated replica document should be readable');
$replicaUpdatedJson = test_assert_json_response($replicaUpdated, 'Updated replica document');
test_assert_same($primaryUpdatedJson['timestamp'] ?? null, $replicaUpdatedJson['timestamp'] ?? null, 'Replicated update timestamp should match primary');
test_assert_same($primaryUpdatedJson['created_at'] ?? null, $replicaUpdatedJson['created_at'] ?? null, 'Replicated update creation time should match primary');

$vector = test_daemon_request(
    'POST',
    "/collections/{$collection}/vector_search?distributed=off",
    '{"field_name":"embedding","vector":[0,1,0],"topk":1}'
);
test_assert($vector !== null && $vector['status'] === 200, 'Read-only replica should serve POST vector searches');
test_assert_json_response($vector, 'Replica vector search');

putenv('HLQUERY_URL=' . $primaryUrl);
for ($index = 0; $index < 12; ++$index) {
    $tieAdd = test_daemon_request(
        'POST',
        "/collections/{$collection}/documents",
        json_encode([
            'id' => sprintf('tie-%02d', $index),
            'title' => 'equal score token',
            'content' => 'deterministic ordering',
            'embedding' => [1, 0, 0],
        ], JSON_THROW_ON_ERROR)
    );
    test_assert($tieAdd !== null && in_array($tieAdd['status'], [200, 201], true), 'Primary tied-score insert should succeed');
}

$orderedHits = static function (array $response): array {
    $json = test_assert_json_response($response, 'Tied-score search');
    $hits = $json['hits'] ?? [];
    test_assert(is_array($hits), 'Tied-score search should return a hits array');

    return array_map(static function (array $hit): array {
        $document = $hit['document'] ?? [];
        return [
            'id' => $document['id'] ?? null,
            'score' => $hit['score'] ?? null,
        ];
    }, $hits);
};

$primaryTieSearch = test_daemon_get(
    "/collections/{$collection}/search?q=equal%20score%20token&query_by=title&limit=100&distributed=off"
);
test_assert($primaryTieSearch !== null && $primaryTieSearch['status'] === 200, 'Primary tied-score search should succeed');
$primaryTieHits = $orderedHits($primaryTieSearch);

putenv('HLQUERY_URL=' . $replicaUrl);
$replicaTieSearch = test_daemon_get(
    "/collections/{$collection}/search?q=equal%20score%20token&query_by=title&limit=100&distributed=off"
);
test_assert($replicaTieSearch !== null && $replicaTieSearch['status'] === 200, 'Replica tied-score search should succeed');
test_assert_same($primaryTieHits, $orderedHits($replicaTieSearch), 'Tied search ordering and scores should match primary');

putenv('HLQUERY_URL=' . $primaryUrl);
$cleanup = test_daemon_request('DELETE', "/collections/{$collection}");
test_assert($cleanup !== null && in_array($cleanup['status'], [200, 204], true), 'Replication test collection cleanup should succeed');

echo "replica_readonly: ok\n";
