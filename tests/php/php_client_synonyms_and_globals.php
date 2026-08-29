<?php

declare(strict_types=1);

require_once __DIR__ . '/bootstrap.php';
require_once test_root('etc/api/php/lib/autoload.php');

final class SynonymRecordingClient extends \Hlquery\Client
{
    /** @var array{method: string, path: string, payload: mixed, query: array}|null */
    public ?array $lastRequest = null;

    public function __construct()
    {
    }

    public function executeRequest($method, $path, $payload = null, array $query = [])
    {
        $this->lastRequest = [
            'method' => $method,
            'path' => $path,
            'payload' => $payload,
            'query' => $query,
        ];

        return $this->lastRequest;
    }
}

$client = new SynonymRecordingClient();
$synonyms = $client->synonyms;
test_assert($synonyms instanceof \Hlquery\Synonyms, 'Client should expose the synonyms service');
test_assert($client->synonymSets === $synonyms, 'synonymSets should alias the synonyms service');

$assertRequest = static function (
    array $expected,
    SynonymRecordingClient $client,
    string $message
): void {
    test_assert_same($expected, $client->lastRequest, $message);
};

$synonyms->list('book_collection', ['sort_by' => 'id']);
$assertRequest(
    ['method' => 'GET', 'path' => '/collections/book_collection/synonyms', 'payload' => null, 'query' => ['sort_by' => 'id']],
    $client,
    'Collection synonym listing should pass query parameters'
);

$payload = ['root' => 'car', 'synonyms' => ['automobile']];
$synonyms->create('book_collection', 'road vehicle', $payload);
$assertRequest(
    ['method' => 'POST', 'path' => '/collections/book_collection/synonyms/road%20vehicle', 'payload' => $payload, 'query' => []],
    $client,
    'Collection synonym creation should encode the synonym id'
);

$synonyms->upsert('book_collection', 'road vehicle', $payload);
$assertRequest(
    ['method' => 'PUT', 'path' => '/collections/book_collection/synonyms/road%20vehicle', 'payload' => $payload, 'query' => []],
    $client,
    'Collection synonym upsert should use PUT'
);

$synonyms->get('book_collection', 'road vehicle');
$assertRequest(
    ['method' => 'GET', 'path' => '/collections/book_collection/synonyms/road%20vehicle', 'payload' => null, 'query' => []],
    $client,
    'Collection synonym lookup should encode the synonym id'
);

$synonyms->delete('book_collection', 'road vehicle');
$assertRequest(
    ['method' => 'DELETE', 'path' => '/collections/book_collection/synonyms/road%20vehicle', 'payload' => null, 'query' => []],
    $client,
    'Collection synonym deletion should encode the synonym id'
);

$client->listSynonymSets(['sort_order' => 'desc']);
$assertRequest(
    ['method' => 'GET', 'path' => '/synonym_sets', 'payload' => null, 'query' => ['sort_order' => 'desc']],
    $client,
    'Client synonym-set listing should use the compatibility route'
);

$client->listGlobalSynonymSet(['sort_by' => 'root']);
$assertRequest(
    ['method' => 'GET', 'path' => '/synonym_sets/global', 'payload' => null, 'query' => ['sort_by' => 'root']],
    $client,
    'Client global synonym-set listing should pass query parameters'
);

$synonyms->createGlobal('smart phone', $payload);
$assertRequest(
    ['method' => 'POST', 'path' => '/synonyms/global/smart%20phone', 'payload' => $payload, 'query' => []],
    $client,
    'Global synonym creation should use the canonical route and encode its id'
);

$synonyms->updateInGlobalSynonymSet('smart phone', $payload);
$assertRequest(
    ['method' => 'PUT', 'path' => '/synonym_sets/global/items/smart%20phone', 'payload' => $payload, 'query' => []],
    $client,
    'Global synonym-set update should use the items route and encode its id'
);

$synonyms->getFromGlobalSynonymSet('smart phone');
$assertRequest(
    ['method' => 'GET', 'path' => '/synonym_sets/global/items/smart%20phone', 'payload' => null, 'query' => []],
    $client,
    'Global synonym-set lookup should use the items route and encode its id'
);

$synonyms->deleteGlobal('smart phone');
$assertRequest(
    ['method' => 'DELETE', 'path' => '/synonyms/global/smart%20phone', 'payload' => null, 'query' => []],
    $client,
    'Global synonym deletion should use the canonical route and encode its id'
);

$client->searchAll(['q' => 'automobile', 'collections' => ['cars']]);
$assertRequest(
    ['method' => 'GET', 'path' => '/search', 'payload' => null, 'query' => ['q' => 'automobile', 'collections' => ['cars']]],
    $client,
    'Global GET search should send parameters in the query string'
);

$client->globalSearch(['q' => 'automobile', 'collections' => ['cars']], 'post');
$assertRequest(
    ['method' => 'POST', 'path' => '/search', 'payload' => ['q' => 'automobile', 'collections' => ['cars']], 'query' => []],
    $client,
    'Global POST search should send parameters in the request body'
);

echo "php_client_synonyms_and_globals: ok\n";
