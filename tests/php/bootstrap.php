<?php

declare(strict_types=1);

function test_assert(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function test_root(string $relative): string
{
    return dirname(__DIR__, 2) . DIRECTORY_SEPARATOR . $relative;
}

/** @return array{status: int, body: string}|null */
function test_daemon_get(string $path): ?array
{
    $baseUrl = getenv('HLQUERY_URL') ?: 'http://127.0.0.1:9200';
    $headers = ['Accept: application/json'];
    $apiKey = getenv('HLQUERY_API_KEY');
    if (is_string($apiKey) && $apiKey !== '') {
        $headers[] = 'Authorization: Bearer ' . $apiKey;
        $headers[] = 'X-API-Key: ' . $apiKey;
    }

    $context = stream_context_create([
        'http' => [
            'method' => 'GET',
            'header' => implode("\r\n", $headers),
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

    return ['status' => isset($matches[1]) ? (int) $matches[1] : 0, 'body' => $body];
}
