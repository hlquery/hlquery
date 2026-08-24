<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$response = test_daemon_get('/status');
if ($response === null) {
    echo "daemon_status: skipped (daemon unavailable)\n";
    exit(0);
}

test_assert($response['status'] === 200, 'Status endpoint should return HTTP 200');
$json = json_decode($response['body'], true);
test_assert(is_array($json), 'Status endpoint should return JSON');
test_assert(($json['status'] ?? null) === 'ok', 'Daemon status should be ok');
test_assert(isset($json['stats']) && is_array($json['stats']), 'Status response should include stats');

echo "daemon_status: ok\n";
