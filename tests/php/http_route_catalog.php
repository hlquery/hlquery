<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$path = test_root('src/api/httproutes.cpp');
$source = file_get_contents($path);
test_assert(is_string($source) && $source !== '', 'HTTP route catalog should be readable');

preg_match_all('/Describe\(RouteAction::([A-Za-z0-9_]+),\s*"([^"]+)"/', $source, $matches, PREG_SET_ORDER);
test_assert(count($matches) >= 50, 'HTTP route catalog should expose the expected breadth of API actions');

$names = [];
foreach ($matches as $match) {
    $action = $match[1];
    $name = $match[2];
    test_assert(!isset($names[$name]), "Route description name {$name} should be unique");
    $names[$name] = $action;
}

foreach (['status', 'health', 'ready', 'collections_list', 'document_search', 'vector_search', 'aliases_list', 'users_list', 'keys_list', 'modules_list'] as $required) {
    test_assert(isset($names[$required]), "Route catalog should contain {$required}");
}

foreach (['/health', '/ready', '/collections', '/multi_search', '/metrics', '/etc'] as $route) {
    test_assert_contains('"' . $route . '"', $source, "Route catalog should contain {$route}");
}

echo "http_route_catalog: ok\n";
