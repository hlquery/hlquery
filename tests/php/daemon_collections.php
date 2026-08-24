<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$response = test_daemon_get('/collections');
if ($response === null) {
    echo "daemon_collections: skipped (daemon unavailable)\n";
    exit(0);
}
if ($response['status'] === 401 && getenv('HLQUERY_API_KEY') === false) {
    echo "daemon_collections: skipped (set HLQUERY_API_KEY for authenticated daemon)\n";
    exit(0);
}

test_assert($response['status'] === 200, 'Collections endpoint should return HTTP 200');
$json = test_assert_json_response($response, 'Collections endpoint');
test_assert(is_array($json), 'Collections endpoint should return a JSON array or object');

echo "daemon_collections: ok\n";
