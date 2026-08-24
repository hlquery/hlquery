<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$binaryDir = test_root('build/bin');
$names = ['hlquery', 'hlquery-cli', 'hlquery-benchmark', 'hlquery-talk'];
$present = array_filter($names, static fn (string $name): bool => is_file($binaryDir . DIRECTORY_SEPARATOR . $name));

if ($present === []) {
    echo "compiled_artifacts: skipped (project has not been built)\n";
    exit(0);
}

foreach ($names as $name) {
    $path = $binaryDir . DIRECTORY_SEPARATOR . $name;
    test_assert(is_file($path), "Compiled artifact {$name} should exist after a build");
    test_assert(is_executable($path), "Compiled artifact {$name} should be executable");
    test_assert(filesize($path) > 4, "Compiled artifact {$name} should not be empty");
    $handle = fopen($path, 'rb');
    test_assert($handle !== false, "Compiled artifact {$name} should be readable");
    $magic = fread($handle, 4);
    fclose($handle);
    test_assert_same("\x7fELF", $magic, "Compiled artifact {$name} should be an ELF binary");
}

echo "compiled_artifacts: ok\n";
