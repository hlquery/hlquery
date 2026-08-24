<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';

$wrapper = test_root('run/hlquery');
if (!is_file($wrapper)) {
    echo "wrapper_commands: skipped (run ./configure to generate run/hlquery)\n";
    exit(0);
}

test_assert(is_executable($wrapper), 'hlquery wrapper should be executable');

$help = test_command([$wrapper, '--help']);
test_assert_same(0, $help['code'], 'hlquery --help should succeed');
test_assert_contains('Usage:', $help['output'], 'Help should contain usage information');
foreach (['start', 'stop', 'status', 'cli', 'benchmark', 'talk', 'test'] as $command) {
    test_assert_contains($command, $help['output'], "Help should document the {$command} command");
}

$invalid = test_command([$wrapper, 'definitely-not-a-command']);
test_assert($invalid['code'] !== 0, 'An unknown wrapper command should fail');
test_assert_contains('Unknown command', $invalid['output'], 'An unknown wrapper command should explain the error');

$version = test_command([$wrapper, '--json', '--version']);
test_assert_same(0, $version['code'], 'hlquery JSON version command should succeed');
$versionJson = test_decode_json($version['output'], 'hlquery JSON version output');
test_assert_same(true, $versionJson['success'] ?? null, 'JSON version output should report success');
test_assert(isset($versionJson['version']) && preg_match('/^\d+\.\d+\.\d+/', $versionJson['version']) === 1, 'JSON version output should contain a semantic version');

if (is_executable(test_root('build/bin/hlquery-cli'))) {
    $cliHelp = test_command([$wrapper, 'cli', 'help']);
    test_assert_same(0, $cliHelp['code'], 'Compiled CLI help should succeed through the wrapper');
    test_assert_contains('COLLECTIONS:', $cliHelp['output'], 'CLI help should document collection commands');
    test_assert_contains('DOCUMENTS:', $cliHelp['output'], 'CLI help should document document commands');

    $benchmarkHelp = test_command([$wrapper, 'benchmark', '--help']);
    test_assert_same(0, $benchmarkHelp['code'], 'Compiled benchmark help should succeed through the wrapper');
    test_assert_contains('--detailed', $benchmarkHelp['output'], 'Benchmark help should document detailed mode');
}

echo "wrapper_commands: ok\n";
