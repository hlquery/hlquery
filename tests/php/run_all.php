<?php

declare(strict_types=1);

$testDir = __DIR__;
$files = glob($testDir . '/*.php') ?: [];
sort($files);

$runner = basename(__FILE__);
$executed = 0;

foreach ($files as $file) {
    if (basename($file) === $runner || basename($file) === 'bootstrap.php') {
        continue;
    }

    $executed++;
    $cmd = escapeshellarg(PHP_BINARY) . ' ' . escapeshellarg($file);
    passthru($cmd, $code);
    if ($code !== 0) {
        fwrite(STDERR, basename($file) . " failed with exit code {$code}\n");
        exit($code);
    }
}

if ($executed === 0) {
    fwrite(STDERR, "No PHP tests found in {$testDir}\n");
    exit(1);
}

echo "php tests completed: {$executed}\n";

