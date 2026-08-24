<?php

declare(strict_types=1);

function test_assert(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function test_assert_same(mixed $expected, mixed $actual, string $message): void
{
    if ($expected !== $actual) {
        throw new RuntimeException(
            $message . '\nExpected: ' . var_export($expected, true) .
            '\nActual:   ' . var_export($actual, true)
        );
    }
}

function test_assert_contains(string $needle, string $haystack, string $message): void
{
    test_assert(str_contains($haystack, $needle), $message);
}

function test_root(string $relative): string
{
    return dirname(__DIR__, 2) . DIRECTORY_SEPARATOR . $relative;
}

/** @return array{code: int, output: string} */
function test_command(array $arguments): array
{
    $command = implode(' ', array_map('escapeshellarg', $arguments)) . ' 2>&1';
    exec($command, $lines, $code);

    return ['code' => $code, 'output' => implode("\n", $lines)];
}

/** @return mixed */
function test_decode_json(string $body, string $description = 'Response')
{
    try {
        return json_decode($body, true, 512, JSON_THROW_ON_ERROR);
    } catch (JsonException $exception) {
        throw new RuntimeException("{$description} should contain valid JSON: {$exception->getMessage()}");
    }
}

/** @return array{status: int, body: string, headers: list<string>}|null */
function test_daemon_request(string $method, string $path, ?string $body = null): ?array
{
    $baseUrl = getenv('HLQUERY_URL') ?: 'http://127.0.0.1:9200';
    $headers = ['Accept: application/json'];
    $apiKey = getenv('HLQUERY_API_KEY');
    if (is_string($apiKey) && $apiKey !== '') {
        $headers[] = 'Authorization: Bearer ' . $apiKey;
        $headers[] = 'X-API-Key: ' . $apiKey;
    }
    if ($body !== null) {
        $headers[] = 'Content-Type: application/json';
    }

    $context = stream_context_create([
        'http' => [
            'method' => $method,
            'header' => implode("\r\n", $headers),
            'content' => $body ?? '',
            'timeout' => 3,
            'ignore_errors' => true,
        ],
    ]);

    $body = @file_get_contents(rtrim($baseUrl, '/') . '/' . ltrim($path, '/'), false, $context);
    if ($body === false) {
        return null;
    }

    $statusLine = $http_response_header[0] ?? '';
    preg_match('/\s(\d{3})\s/', $statusLine, $matches);

    return [
        'status' => isset($matches[1]) ? (int) $matches[1] : 0,
        'body' => $body,
        'headers' => array_values($http_response_header ?? []),
    ];
}

/** @return array{status: int, body: string, headers: list<string>}|null */
function test_daemon_get(string $path): ?array
{
    return test_daemon_request('GET', $path);
}

/** @param array{status: int, body: string, headers: list<string>} $response */
function test_assert_json_response(array $response, string $description): mixed
{
    $contentType = implode("\n", $response['headers']);
    test_assert(
        preg_match('/^Content-Type:\s*application\/json\b/im', $contentType) === 1,
        "{$description} should use the application/json content type"
    );

    return test_decode_json($response['body'], $description);
}
