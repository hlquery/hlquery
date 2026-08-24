<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$health = test_daemon_get('/health');
if ($health === null) {
    echo "daemon_public_endpoints: skipped (daemon unavailable)\n";
    exit(0);
}

$expectations = [
    '/ping' => [200],
    '/ready' => [200, 503],
    '/health' => [200, 503],
    '/status' => [200],
    '/etc' => [200],
];

foreach ($expectations as $path => $statuses) {
    $response = test_daemon_get($path);
    test_assert($response !== null, "{$path} should remain reachable during the test");
    test_assert(in_array($response['status'], $statuses, true), "{$path} returned unexpected HTTP status {$response['status']}");
    $json = test_assert_json_response($response, $path);
    test_assert(is_array($json), "{$path} should return a JSON object or array");
}

$withQuery = test_daemon_get('/health?php_test=1');
test_assert($withQuery !== null, 'Health route with a query string should be reachable');
test_assert(in_array($withQuery['status'], [200, 503], true), 'Route matching should ignore the query string');
test_assert_json_response($withQuery, 'Health route with query string');

$missing = test_daemon_get('/__php_test_route_that_does_not_exist__');
test_assert($missing !== null, 'Unknown route request should receive a response');
test_assert_same(404, $missing['status'], 'An unknown route should return HTTP 404');
$missingJson = test_assert_json_response($missing, 'Unknown route');
test_assert(isset($missingJson['error']), 'An unknown route should return a JSON error');

echo "daemon_public_endpoints: ok\n";
