<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$response = test_daemon_get('/health');
if ($response === null) {
    echo "daemon_health: skipped (daemon unavailable)\n";
    exit(0);
}

test_assert(in_array($response['status'], [200, 503], true), 'Health endpoint should return HTTP 200 or 503');

$json = test_assert_json_response($response, 'Health endpoint');
test_assert(is_array($json), 'Health endpoint should return JSON');
test_assert(array_key_exists('status', $json) || array_key_exists('health', $json), 'Health response should expose a status field');

echo "daemon_health: ok\n";
