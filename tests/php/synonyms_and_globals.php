<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$collections = test_daemon_get('/collections');
if ($collections === null) {
    echo "synonyms_and_globals: skipped (daemon unavailable)\n";
    exit(0);
}
if ($collections['status'] === 401 && getenv('HLQUERY_API_KEY') === false) {
    echo "synonyms_and_globals: skipped (set HLQUERY_API_KEY for authenticated daemon)\n";
    exit(0);
}
test_assert_same(200, $collections['status'], 'Collections endpoint should be available before testing synonyms');

$suffix = (string) getmypid() . bin2hex(random_bytes(4));
$collection = 'php_synonyms_' . $suffix;
$localId = 'local_' . $suffix;
$globalId = 'global_' . $suffix;
$localRoot = 'localroot' . $suffix;
$localAlias = 'localalias' . $suffix;
$globalRoot = 'globalroot' . $suffix;
$globalAlias = 'globalalias' . $suffix;
$globalCreated = false;
$collectionCreated = false;
$primaryUrl = getenv('HLQUERY_URL') ?: 'http://127.0.0.1:9200';
$replicaUrl = getenv('HLQUERY_REPLICA_URL');

/** @return list<string> */
$hitIds = static function (array $response, string $description): array {
    $json = test_assert_json_response($response, $description);
    $hits = $json['hits'] ?? null;
    test_assert(is_array($hits), "{$description} should return a hits array");

    return array_values(array_filter(array_map(static function ($hit): ?string {
        if (!is_array($hit)) {
            return null;
        }

        $document = $hit['document'] ?? null;
        if (is_array($document) && is_string($document['id'] ?? null)) {
            return $document['id'];
        }

        return is_string($hit['id'] ?? null) ? $hit['id'] : null;
    }, $hits)));
};

$replicaRequest = static function (string $method, string $path, ?string $body = null) use ($primaryUrl, $replicaUrl): ?array {
    if (!is_string($replicaUrl) || $replicaUrl === '') {
        return null;
    }

    putenv('HLQUERY_URL=' . $replicaUrl);
    try {
        return test_daemon_request($method, $path, $body);
    } finally {
        putenv('HLQUERY_URL=' . $primaryUrl);
    }
};

try {
    $createCollection = test_daemon_request('POST', '/collections', json_encode([
        'name' => $collection,
        'fields' => [
            ['name' => 'title', 'type' => 'string'],
        ],
    ], JSON_THROW_ON_ERROR));
    test_assert(
        $createCollection !== null && in_array($createCollection['status'], [200, 201], true),
        'Synonym test collection should be created'
    );
    $collectionCreated = true;

    foreach ([
        ['id' => 'local-document', 'title' => $localRoot],
        ['id' => 'global-document', 'title' => $globalRoot],
    ] as $document) {
        $createDocument = test_daemon_request(
            'POST',
            "/collections/{$collection}/documents",
            json_encode($document, JSON_THROW_ON_ERROR)
        );
        test_assert(
            $createDocument !== null && in_array($createDocument['status'], [200, 201], true),
            "Document {$document['id']} should be created"
        );
    }

    $createLocal = test_daemon_request(
        'PUT',
        "/collections/{$collection}/synonyms/{$localId}",
        json_encode(['root' => $localRoot, 'synonyms' => [$localAlias]], JSON_THROW_ON_ERROR)
    );
    test_assert_same(200, $createLocal['status'] ?? null, 'Collection synonym should be upserted');
    $createLocalJson = test_assert_json_response($createLocal, 'Collection synonym upsert');
    test_assert_same($localId, $createLocalJson['id'] ?? null, 'Collection synonym upsert should return its id');
    test_assert_same('collection', $createLocalJson['scope'] ?? null, 'Collection synonym should report collection scope');

    $getLocal = test_daemon_get("/collections/{$collection}/synonyms/{$localId}");
    test_assert_same(200, $getLocal['status'] ?? null, 'Collection synonym should be readable');
    $getLocalJson = test_assert_json_response($getLocal, 'Collection synonym');
    test_assert_same($localRoot, $getLocalJson['root'] ?? null, 'Collection synonym should preserve its root');
    test_assert_same([$localAlias], $getLocalJson['synonyms'] ?? null, 'Collection synonym should preserve its aliases');

    $listLocal = test_daemon_get("/collections/{$collection}/synonyms?sort_by=id&sort_order=asc");
    test_assert_same(200, $listLocal['status'] ?? null, 'Collection synonyms should be listable');
    $listLocalJson = test_assert_json_response($listLocal, 'Collection synonym list');
    test_assert_same('collection', $listLocalJson['scope'] ?? null, 'Collection synonym list should report collection scope');
    test_assert_same(1, $listLocalJson['count'] ?? null, 'Collection synonym list should include the created group');
    test_assert_same($localId, $listLocalJson['synonyms'][0]['id'] ?? null, 'Collection synonym list should contain the created id');

    $localSearch = test_daemon_get(
        "/collections/{$collection}/search?q=" . rawurlencode($localAlias) . '&query_by=title&enable_synonyms=true&distributed=off'
    );
    test_assert_same(200, $localSearch['status'] ?? null, 'Search using a collection synonym should succeed');
    test_assert(
        in_array('local-document', $hitIds($localSearch, 'Collection synonym search'), true),
        'Searching for a collection alias should find the document containing its root term'
    );

    if (is_string($replicaUrl) && $replicaUrl !== '') {
        $replicaLocal = $replicaRequest('GET', "/collections/{$collection}/synonyms/{$localId}?distributed=off");
        test_assert_same(200, $replicaLocal['status'] ?? null, 'Replica should receive the collection synonym');
        $replicaLocalJson = test_assert_json_response($replicaLocal, 'Replicated collection synonym');
        test_assert_same($localRoot, $replicaLocalJson['root'] ?? null, 'Replicated collection synonym should preserve its root');

        $replicaLocalSearch = $replicaRequest(
            'GET',
            "/collections/{$collection}/search?q=" . rawurlencode($localAlias) . '&query_by=title&enable_synonyms=true&distributed=off'
        );
        test_assert_same(200, $replicaLocalSearch['status'] ?? null, 'Replica collection synonym search should succeed');
        test_assert(
            in_array('local-document', $hitIds($replicaLocalSearch, 'Replica collection synonym search'), true),
            'Replica should expand collection synonyms during search'
        );
    }

    $createGlobal = test_daemon_request(
        'POST',
        "/synonym_sets/global/items/{$globalId}",
        json_encode(['root' => $globalRoot, 'synonyms' => [$globalAlias]], JSON_THROW_ON_ERROR)
    );
    test_assert_same(200, $createGlobal['status'] ?? null, 'Global synonym should be created through the synonym-set route');
    $globalCreated = true;
    $createGlobalJson = test_assert_json_response($createGlobal, 'Global synonym upsert');
    test_assert_same($globalId, $createGlobalJson['id'] ?? null, 'Global synonym upsert should return its id');
    test_assert_same('global', $createGlobalJson['scope'] ?? null, 'Global synonym should report global scope');

    $getGlobal = test_daemon_get("/synonyms/global/{$globalId}");
    test_assert_same(200, $getGlobal['status'] ?? null, 'Global synonym should be readable through the canonical route');
    $getGlobalJson = test_assert_json_response($getGlobal, 'Global synonym');
    test_assert_same($globalRoot, $getGlobalJson['root'] ?? null, 'Global synonym should preserve its root');
    test_assert_same([$globalAlias], $getGlobalJson['synonyms'] ?? null, 'Global synonym should preserve its aliases');

    $listGlobal = test_daemon_get('/synonym_sets/global?sort_by=id&sort_order=asc');
    test_assert_same(200, $listGlobal['status'] ?? null, 'Global synonym set should be listable');
    $listGlobalJson = test_assert_json_response($listGlobal, 'Global synonym list');
    test_assert_same('global', $listGlobalJson['scope'] ?? null, 'Global synonym list should report global scope');
    $globalIds = array_column($listGlobalJson['synonyms'] ?? [], 'id');
    test_assert(in_array($globalId, $globalIds, true), 'Global synonym list should contain the created id');

    $globalSearch = test_daemon_get(
        "/collections/{$collection}/search?q=" . rawurlencode($globalAlias) . '&query_by=title&enable_synonyms=true&distributed=off'
    );
    test_assert_same(200, $globalSearch['status'] ?? null, 'Search using a global synonym should succeed');
    test_assert(
        in_array('global-document', $hitIds($globalSearch, 'Global synonym search'), true),
        'Searching for a global alias should find the document containing its root term'
    );

    if (is_string($replicaUrl) && $replicaUrl !== '') {
        $replicaGlobal = $replicaRequest('GET', "/synonyms/global/{$globalId}");
        test_assert_same(200, $replicaGlobal['status'] ?? null, 'Replica should receive the global synonym');
        $replicaGlobalJson = test_assert_json_response($replicaGlobal, 'Replicated global synonym');
        test_assert_same($globalRoot, $replicaGlobalJson['root'] ?? null, 'Replicated global synonym should preserve its root');

        $replicaGlobalSearch = $replicaRequest(
            'GET',
            "/collections/{$collection}/search?q=" . rawurlencode($globalAlias) . '&query_by=title&enable_synonyms=true&distributed=off'
        );
        test_assert_same(200, $replicaGlobalSearch['status'] ?? null, 'Replica global synonym search should succeed');
        test_assert(
            in_array('global-document', $hitIds($replicaGlobalSearch, 'Replica global synonym search'), true),
            'Replica should expand global synonyms during search'
        );
    }

    $deleteGlobal = test_daemon_request('DELETE', "/synonym_sets/global/items/{$globalId}");
    test_assert_same(200, $deleteGlobal['status'] ?? null, 'Global synonym should be deletable through the synonym-set route');
    $globalCreated = false;
    $missingGlobal = test_daemon_get("/synonyms/global/{$globalId}");
    test_assert_same(404, $missingGlobal['status'] ?? null, 'Deleted global synonym should no longer exist');
    if (is_string($replicaUrl) && $replicaUrl !== '') {
        $missingReplicaGlobal = $replicaRequest('GET', "/synonyms/global/{$globalId}");
        test_assert_same(404, $missingReplicaGlobal['status'] ?? null, 'Deleted global synonym should be removed from the replica');
    }

    $deleteLocal = test_daemon_request('DELETE', "/collections/{$collection}/synonyms/{$localId}");
    test_assert_same(200, $deleteLocal['status'] ?? null, 'Collection synonym should be deletable');
    $missingLocal = test_daemon_get("/collections/{$collection}/synonyms/{$localId}");
    test_assert_same(404, $missingLocal['status'] ?? null, 'Deleted collection synonym should no longer exist');
    if (is_string($replicaUrl) && $replicaUrl !== '') {
        $missingReplicaLocal = $replicaRequest('GET', "/collections/{$collection}/synonyms/{$localId}?distributed=off");
        test_assert_same(404, $missingReplicaLocal['status'] ?? null, 'Deleted collection synonym should be removed from the replica');
    }
} finally {
    if ($globalCreated) {
        test_daemon_request('DELETE', "/synonym_sets/global/items/{$globalId}");
    }
    if ($collectionCreated) {
        test_daemon_request('DELETE', "/collections/{$collection}");
    }
}

echo "synonyms_and_globals: ok\n";
