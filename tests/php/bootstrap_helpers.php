<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

test_assert(true, 'A true assertion should pass');
test_assert_same(['one' => 1], test_decode_json('{"one":1}', 'Fixture'), 'JSON helper should decode objects');
test_assert_contains('query', test_root('tests/php/bootstrap.php'), 'Root paths should retain their suffix');
test_assert_same(
    dirname(__DIR__, 2) . DIRECTORY_SEPARATOR . 'README.md',
    test_root('README.md'),
    'Root paths should resolve from the repository root'
);

$command = test_command([PHP_BINARY, '-r', 'fwrite(STDERR, "stderr"); echo "stdout"; exit(7);']);
test_assert_same(7, $command['code'], 'Command helper should preserve a non-zero exit code');
test_assert_contains('stdout', $command['output'], 'Command helper should capture stdout');
test_assert_contains('stderr', $command['output'], 'Command helper should capture stderr');

echo "bootstrap_helpers: ok\n";
