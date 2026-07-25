> **Development Status**: hlquery is currently in active development and should not be used in production environments. The software may contain bugs and incomplete features, and breaking changes may occur without notice.

<div align="center">
  <img src="https://docs.hlquery.com/img/hlquery/2.png" alt="hlquery logo" width="200">
</div>

<div align="center">

**A modular, high-performance search engine built for modern applications.**

[![Follow hlquery](https://img.shields.io/badge/Follow-%40hlquery-blue?logo=x&logoColor=white&labelColor=000000)](https://x.com/hlquery)
[![GitHub](https://img.shields.io/badge/GitHub-hlquery-blue?logo=github&logoColor=white&labelColor=000000)](https://github.com/hlquery/hlquery/)
[![Linux Build](https://img.shields.io/badge/Linux%20Build-passing-brightgreen?logo=linux&logoColor=white&labelColor=000000)](https://github.com/hlquery/hlquery/actions)
[![macOS Build](https://img.shields.io/badge/macOS%20Build-passing-brightgreen?logo=apple&logoColor=white&labelColor=000000)](https://github.com/hlquery/hlquery/actions)
[![FreeBSD Build](https://img.shields.io/badge/FreeBSD%20Build-passing-brightgreen?logo=freebsd&logoColor=white&labelColor=000000)](https://github.com/hlquery/hlquery/actions)
[![Demo](https://img.shields.io/badge/Demo-live-0ea5e9?logo=google-chrome&logoColor=white&labelColor=000000)](https://demo.hlquery.com/)
[![License](https://img.shields.io/badge/License-BSD%203--Clause-a35a0f?logo=open-source-initiative&logoColor=white&labelColor=000000)](https://opensource.org/licenses/BSD-3-Clause)

</div>

> You can explore the live demo at [demo.hlquery.com](https://demo.hlquery.com/).
> It runs on the [m_demo.cpp](https://github.com/hlquery/hlquery/blob/unstable/src/modules/m_demo.cpp) module, which disables insert, delete, and update operations in demo mode.
> The demo UI is built with [hanalyzer](https://github.com/hlquery/hanalyzer).

### What is hlquery?

hlquery is an open-source C++ search engine built to stay lightweight while handling millions of results efficiently. It targets applications that need fast indexing, real-time queries, and a straightforward HTTP/JSON interface with advanced search features. The engine supports full-text search, hybrid ranking, vector similarity, flexible collections, and configurable runtime modules for features such as AI-assisted search.
It also includes a REST API for indexing and querying, plus command-line tools for local management and testing.

### Why choose hlquery?

hlquery is built for teams that want strong search without the operational weight of a larger stack. It combines fast indexing, low-latency queries, and a simple HTTP/JSON API with features usually found in more complex systems.

You can use hlquery for full-text search, hybrid retrieval, vector similarity, and AI-assisted workflows while keeping deployment straightforward. It ships with client libraries, command-line tools, and modular runtime extensions for local development and production services.

### Data organization

hlquery organizes data into **collections**. A collection is a logical group of related records, such as products, articles, users, or events, and its schema defines the fields that can be indexed, searched, filtered, sorted, or used for faceting.

Each collection contains **documents**. A document is a JSON record identified by a unique ID and made up of fields such as `title`, `content`, `category`, or `price`. Documents in the same collection follow the same general schema, while each document stores its own values.

For example, a `products` collection can contain one document per product. You can then search the text fields, filter by structured fields such as category or price, and return only the fields needed by your application. Collections keep different types of data organized while allowing each type to have its own schema and search behavior.

### Prerequisites

**Debian/Ubuntu:**
```bash
$ sudo apt-get install build-essential cmake libssl-dev liburing-dev
```

If CMake prints a `uring` lookup warning during the rocksdb build, it usually means the `liburing` development package is missing. Installing `liburing-dev` on Debian/Ubuntu provides the package metadata CMake is looking for and clears the warning.

**Red Hat/CentOS:**
```bash
$ sudo dnf install @development-tools cmake openssl-devel
```

**macOS:**
```bash
$ xcode-select --install
$ brew install cmake openssl
```

On macOS, Xcode Command Line Tools provide the C/C++ compiler and `make`.
Homebrew provides CMake and OpenSSL.

**FreeBSD:**
```bash
$ sudo pkg install gmake cmake openssl
```

**Note**: This project uses gmake features. On FreeBSD, run `gmake` instead of `make`.

### Installation

```
$ wget https://github.com/hlquery/hlquery/archive/refs/heads/unstable.zip
$ cd hlquery/
$ ./configure
```

On GNU/Linux:

```
$ make -j4
$ make install
```

On FreeBSD, use GNU make for the build and install steps:

```
$ gmake -j4
$ gmake install
```

### Running hlquery

**Start the server**

```
$ ./run/hlquery start
[ OK ] Starting hlquery: [Jul-12 - 12:37:59]
...
```

> **Note**: hlquery uses port **9200** by default. Ensure this port is available and not blocked by your firewall.

**Stop the server**

```
$ ./run/hlquery stop
[ INFO ] Stopping hlquery (PID: 27008) ...
[ OK ] hlquery stopped successfully.
```

**Stop the server as JSON**

```
$ ./run/hlquery stop --json
{"action":"stop","stopped_pid":206773,"success":true}
```

**Run in foreground (for debugging):**

```
$ ./run/hlquery start --nofork
...
```

**Run the interactive shell**

```
$ ./run/hlquery talk
localhost:9200> use art
Using collection 'art'.
```

**Checking uptime's**
```
localhost:9200|art> uptime
Server up for 3 days, 1h 0m 31s
```

### Client Libraries

Official client libraries are available for popular programming languages:

| Client | Description |
| --- | --- |
| **[C++](https://github.com/hlquery/cpp-api)** | Native C++ client library for low-level and embedded integrations. |
| **[Go](https://github.com/hlquery/go-api)** | Go client for indexing, search, and service backends. |
| **[Java](https://github.com/hlquery/java-api)** | JVM client for Java applications and server-side integrations. |
| **[Node.js](https://github.com/hlquery/node-api)** | Async JavaScript client for Node.js services and tools. |
| **[Perl](https://github.com/hlquery/perl-api)** | Perl client library for scripts and existing Perl services. |
| **[PHP](https://github.com/hlquery/php-api)** | Composer-ready PHP client for web apps and API integrations. |
| **[Python](https://github.com/hlquery/python-api)** | Python client for scripts, data workflows, and backend services. |
| **[Ruby](https://github.com/hlquery/ruby-api)** | Ruby client for Rails apps, scripts, and service integrations. |
| **[Rust](https://github.com/hlquery/rust-api)** | Rust client library for strongly typed hlquery integrations. |
| **[TypeScript](https://github.com/hlquery/type-api)** | Typed client for TypeScript applications and SDK-style integrations. |

For complete API documentation, visit [docs.hlquery.com](https://docs.hlquery.com/).

### Create a Collection

```bash
$ ./run/hlquery cli create products title content price
Collection 'products' created successfully
```

**Using the PHP API:**

```php
<?php

require_once __DIR__ . '/vendor/autoload.php';
use Hlquery\Client;

$client = new Client('http://localhost:9200');

/* Get the collections service from the client. */

$collections = $client->collections;

/* Build the schema payload sent to hlquery. */

$schema = [
    'fields' => [
        /* Keep the main product title searchable.     */

        ['name' => 'title', 'type' => 'string'],

        /* Index the longer product description text.  */

        ['name' => 'content', 'type' => 'string'],

        /* Keep product identifiers as exact, non-fuzzy values. */

        ['name' => 'sku', 'type' => 'keyword'],

        /* Save a numeric price for filters and sorts. */

        ['name' => 'price', 'type' => 'float'],
    ],
];

$response = $collections->create('products', $schema);
$body = $response->getBody();

$client->documents->add('products', [
    'id' => 'prod_keyboard_001',
    'title' => 'Wireless Keyboard',
    'content' => 'Compact Bluetooth keyboard for daily work.',
    'price' => 49.99,
]);

```

### Index Documents

Each document ID must be unique within its collection:

```bash
$ hlquery-cli add products prod_laptop_001 "Laptop Computer" "High-performance laptop with 16GB RAM"
Document 'prod_laptop_001' added to collection 'products'
```

**Using the Node API**

```js
const Client = require('hlquery-node-client');
const client = new Client('http://localhost:9200');
const documents = client.documents(); // Use the documents service for document writes.

/* Send POST /collections/products/documents with the product payload. */

const response = await documents.add('products', {
  id: 'prod_laptop_001',
  title: 'Laptop Computer',
  content: 'High-performance laptop with 16GB RAM',
  price: 1299.99
});

/* Inspect the JSON body returned by the API. */

console.log(response.getBody());

```

### Search

```text
$ hlquery-cli search products "laptop"
Search results for 'laptop' in collection 'products':
Found 1 document(s) (showing 1-1 of 1)

+---+-----------------+----------+-----------------+---------------------------------------+
| # | Document ID     | Score    | Title           | Content Preview                       |
+---+-----------------+----------+-----------------+---------------------------------------+
| 1 | prod_laptop_001 | 1.094500 | Laptop Computer | High-performance laptop with 16GB RAM |
+---+-----------------+----------+-----------------+---------------------------------------+
```

**Using the C++ API**

```cpp
#include "hlquery/client.h"

hlquery::Client client("http://localhost:9200");
auto collections = client.collections();
auto result = collections->search("products", {{"like", "laptop"}});
```


### Example Queries

```bash
# Field-specific search
$ ./run/hlquery cli search products "title:laptop"

# Range query
$ ./run/hlquery cli search products "price:[100 TO 500]"

# Fuzzy search (tolerates typos)
$ ./run/hlquery cli search products "laptop~2"

# Wildcard search
$ ./run/hlquery cli search products "laptop*"

# Case-sensitive search
$ ./run/hlquery cli search products "is:casesensitive Laptop"

# Boost term importance
$ ./run/hlquery cli search products "laptop^2.0 computer"

# NOT operator
$ ./run/hlquery cli search products "!apple"

# Combined queries
$ ./run/hlquery cli search products "title:laptop AND price:[100 TO 500]"
```

### Links, Distributed Search, and Replication

hlquery supports linking two or more servers together. Links are configured in `links.conf` with `<node ...>` entries and can be used for distributed queries, write replication, or both by listing the same remote endpoint with the role needed for each purpose.

Distributed search fans a query out to linked search nodes and merges the results. Use `role="distributed"` for query peers and enable `<distributed_search ...>`:

```text
<node
     host="127.0.0.1"
     port="9201"
     role="distributed"
     passwd="shared-secret">

<distributed_search
     enabled="true"
     mode="local_first"
     prefer_local="true"
     timeout_ms="250">
```

**SQL example:**

```text
$ ./run/hlquery talk
localhost:9200> sql: select title from music where content like 'madon%' or content like 'nirva%';
SQL rows for `select title from music where content like 'madon%' or content like 'nirva%';`:
+-------------------------+
| title                   |
+-------------------------+
| Artist Profile: Madonna |
| Artist Profile: Nirvana |
+-------------------------+
2 results shown.
Search completed in 19 ms.
```

Runtime links use the same role split. Use `role="master"` (or `distributed`) for a query link and `role="slave"` (or `replica`) for a replication target. If the link is authenticated, include `token` and optionally `token2` in the JSON body; runtime-added links are in-memory and must be added again after restart:

```text
POST /links/connect
{"host":"127.0.0.1","port":9202,"role":"slave","token":"shared-secret"}
```

### GitHub Repositories & Branch Structure

hlquery is actively developed across multiple GitHub repositories. We maintain a structured development workflow to ensure stability and continuous improvement.

Each repository follows a **two-branch development model**:

- **`unstable`** - Active development branch where new features, bug fixes, and improvements are developed
- **`1.0`** - Stable release branch containing production-ready code

### Active Development

We are committed to **active, continuous development** of hlquery and all related projects. New features, performance improvements, and bug fixes are regularly added across all repositories.

**Want to stay updated?** ⭐ **Star and watch our repositories on GitHub** to receive notifications about:
- New releases and features
- Bug fixes and improvements
- Documentation updates
- Community discussions

Subscribe to repository notifications to never miss an update!

### Contributing

We welcome contributions from the community! All contributions must be released under the BSD 3-Clause license.

### How to Contribute

- Check existing [issues](https://github.com/hlquery/hlquery/issues) or open a new one.
- Contribute to client libraries.
- Test and report bugs.
- Improve documentation.

### Community

- 📖 [Documentation](https://docs.hlquery.com)
- 🐦 [X (Twitter)](https://x.com/hlquery)
- 📦 [GitHub](https://github.com/hlquery/hlquery)

### License

hlquery is licensed under the [BSD 3-Clause License](https://opensource.org/licenses/BSD-3-Clause).
