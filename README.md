<div align="center">
  <img src="https://docs.hlquery.com/img/hlquery/2.png" alt="hlquery logo" width="200">
</div>

> **Development Status**: hanalyzer is currently in active development and should not be used in production environments. The software may contain bugs, incomplete features, and breaking changes may occur without notice.

<div align="center">

**A modular, high-performance search engine built for modern applications.**

[![Follow hlquery](https://img.shields.io/badge/Follow-%40hlquery-blue?logo=x&logoColor=white)](https://x.com/hlquery)
[![Linux Build](https://github.com/hlquery/hlquery/workflows/Linux%20build/badge.svg)](https://github.com/hlquery/hlquery/actions)
[![macOS Build](https://github.com/hlquery/hlquery/workflows/macOS%20Build/badge.svg)](https://github.com/hlquery/hlquery/actions)
[![FreeBSD Build](https://github.com/hlquery/hlquery/workflows/FreeBSD%20Build/badge.svg)](https://github.com/hlquery/hlquery/actions)
[![Commit Activity](https://img.shields.io/github/commit-activity/m/hlquery/hlquery)](https://github.com/hlquery/hlquery/pulse)
[![GitHub](https://img.shields.io/badge/GitHub-hlquery-blue?logo=github&logoColor=white)](https://github.com/hlquery/hlquery/stargazers)
[![License](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

</div>

> You can explore the live demo at [demo.hlquery.com](https://demo.hlquery.com/).
> Demo mode is powered by [m_demo.cpp](https://github.com/hlquery/hlquery/blob/unstable/src/modules/m_demo.cpp), so insert, delete, and update operations are disabled.
> The demo UI is built with [hanalyzer](https://github.com/hlquery/hanalyzer).

### What is hlquery?

hlquery is an open source search engine written in C++17, optimized to stay lightweight while handling millions of results efficiently. It is designed for applications that need fast indexing, real-time queries, and a straightforward HTTP/JSON interface without giving up advanced search features. The engine supports full-text search, hybrid ranking, vector similarity, flexible collections, and configurable runtime modules for features such as AI-assisted search.
It exposes a REST API for indexing, querying, and administration, and includes command-line tools for local management and testing.

### Why choose hlquery?

hlquery is built for teams that want strong search capabilities without taking on the operational weight of a larger search stack. It combines fast indexing, low-latency queries, and a simple HTTP/JSON surface area with features that are usually expected from more complex systems.

You can use hlquery for classic full-text search, hybrid retrieval, vector similarity, and AI-assisted search workflows while keeping deployment and integration straightforward. The project also ships with official client libraries, command-line tools, and modular runtime extensions, which makes it practical both for local development and production services.

### SAM: Search That Gets Smarter Over Time

hlquery includes **SAM**, the **Secondary Assistant Manager**. SAM adds a second retrieval layer for natural-language intent, search assistance, and learned query behavior, helping hlquery move beyond strict keyword matching when users do not know the exact terms in your data.

That makes SAM a strong option for support search, internal knowledge tools, copilots, and AI-assisted workflows. It expands documents into broader lookup phrases, records repeated search ideas, and can build collection-level intent profiles over time. The result is a search experience that needs less reformulation and handles messier queries more naturally.

Just as important, SAM runs inside hlquery. You do not need a separate service to experiment with smarter retrieval, and you do not have to give up lexical or hybrid precision to use it. Queries like "the error about invalid session token" or "cheap gaming laptop for travel" are exactly where SAM can help close the gap between how people search and how data is stored.

---

### Prerequisites

**Debian/Ubuntu:**
```bash
$ sudo apt-get install build-essential cmake libssl-dev
```

**RedHat/CentOS:**
```bash
$ sudo dnf install @development-tools cmake openssl-devel
```

**macOS:**
```bash
$ xcode-select --install
$ brew install cmake openssl
```

On macOS, Xcode Command Line Tools provide the C/C++ compiler and `make`,
Homebrew provides CMake and OpenSSL.

**FreeBSD:**
```bash
$ sudo pkg install gmake cmake openssl
```

> **Note**: This project uses gmake features. On FreeBSD, run `gmake` instead of `make`.

### Installation

```bash
$ wget https://github.com/hlquery/hlquery/archive/refs/heads/unstable.zip
$ cd hlquery/
$ ./configure
```

On Linux:

```
$ make -j4
$ make install
```

On FreeBSD, use GNU make for the build and install steps:

```bash
$ gmake -j4
$ gmake install
```

### Running hlquery

**Start the server:**

```bash
$ ./run/hlquery start
```

> **Note**: hlquery uses port **9200** by default. Ensure this port is available and not blocked by your firewall.

**Stop the server:**

```bash
$ ./run/hlquery stop
```

**Stop the server as JSON:**

```bash
$ ./run/hlquery stop --json
{"action":"stop","stopped_pid":206773,"success":true}
```

**Run in foreground (for debugging):**

```bash
$ ./run/hlquery start --nofork
```

**Run talk:**

```text
$ ./run/bin/hlquery-talk
localhost:9200> use art
Using collection 'art'.
localhost:9200|art> uptime
Server up for 3 days, 1h 0m 31s
```

### Client Libraries

Official client libraries are available for popular programming languages:

| Client | Description |
| --- | --- |
| [Node.js](https://github.com/hlquery/node-api) | Async JavaScript client for Node.js services and tools. |
| [TypeScript](https://github.com/hlquery/type-api) | Typed client for TypeScript applications and SDK-style integrations. |
| [Go](https://github.com/hlquery/go-api) | Idiomatic Go client for indexing, search, and service backends. |
| [Java](https://github.com/hlquery/java-api) | JVM client for Java applications and server-side integrations. |
| [Python](https://github.com/hlquery/python-api) | Python client for scripts, data workflows, and backend services. |
| [PHP](https://github.com/hlquery/php-api) | Composer-ready PHP client for web apps and API integrations. |
| [Ruby](https://github.com/hlquery/ruby-api) | Ruby client for Rails apps, scripts, and service integrations. |
| [Rust](https://github.com/hlquery/rust-api) | Rust client library for strongly typed hlquery integrations. |
| [Perl](https://github.com/hlquery/perl-api) | Perl client library for scripts and existing Perl services. |
| [C++](https://github.com/hlquery/cpp-api) | Native C++ client library for low-level and embedded integrations. |

For complete API documentation, visit [docs.hlquery.com](https://docs.hlquery.com/).

### Create a Collection

```bash
$ hlquery-cli create products title content price
Collection 'products' created successfully
```

**Using the PHP API:**

```php
<?php

require_once __DIR__ . '/vendor/autoload.php'; 
use Hlquery\Client; 

$client = new Client('http://localhost:9200'));

/* Get the collections service from the client. */

$collections = $client->collections();

/* Build the schema payload sent to hlquery. */

$schema = [ 
    'fields' => [ 
        ['name' => 'id', 'type' => 'string'],        /* Store the document id as a string field.    */
        ['name' => 'title', 'type' => 'string'],     /* Keep the main product title searchable.     */
        ['name' => 'content', 'type' => 'string'],   /* Index the longer product description text.  */
        ['name' => 'price', 'type' => 'float'],      /* Save a numeric price for filters and sorts. */
    ], 
]; 

$response = $collections->create('products', $schema); 
$body = $response->getBody(); 
```

### Index Documents

```bash
$ hlquery-cli add products product1 "Laptop Computer" "High-performance laptop with 16GB RAM"
Document 'product1' added to collection 'products'
```

**Using the Node API:**

```js
const Client = require('hlquery-node-client');
const client = new Client('http://localhost:9200'); 
const documents = client.documents(); // Use the documents service for document writes.

/* Send POST /collections/products/documents with the product payload. */

const response = await documents.add('products', {
  id: 'product1', // Primary document id used by later reads and updates.
  title: 'Laptop Computer', // Searchable title field.
  content: 'High-performance laptop with 16GB RAM', // Main body text to index.
  price: 1299.99 // Numeric field for filtering and sorting.
}); 

/* Inspect the JSON body returned by the API. */

console.log(response.getBody()); 

```

### Search

```text
$ hlquery-cli search products "laptop"
Search results for 'laptop' in collection 'products':
Found 1 document(s) (showing 1-1 of 1)

+---+-------------+----------+-----------------+---------------------------------------+----------+
| # | Document ID | Score    | Title           | Content Preview                       | Fields   |
+---+-------------+----------+-----------------+---------------------------------------+----------+
| 1 | product1    | 1.094500 | Laptop Computer | High-performance laptop with 16GB RAM | 0 fields |
+---+-------------+----------+-----------------+---------------------------------------+----------+
```

**Using the C++ API:**

```cpp
#include "hlquery/client.h"

hlquery::Client client("http://localhost:9200");
auto collections = client.collections();
auto result = collections->search("products", {{"like", "laptop"}});
```

### Example Queries

```bash
# Field-specific search
$ hlquery-cli search products "title:laptop"

# Range query
$ hlquery-cli search products "price:[100 TO 500]"

# Fuzzy search (tolerates typos)
$ hlquery-cli search products "laptop~2"

# Wildcard search
$ hlquery-cli search products "laptop*"

# Case-sensitive search
$ hlquery-cli search products "is:casesensitive Laptop"

# Boost term importance
$ hlquery-cli search products "laptop^2.0 computer"

# NOT operator
$ hlquery-cli search products "!apple"

# Combined queries
$ hlquery-cli search products "title:laptop AND price:[100 TO 500]"
```

**SQL example:**

```text
$ ./run/bin/hlquery-talk
localhost:9200> sql: select title from music where content like 'madonna%' or content like 'nirvana%';
SQL rows for `select title from music where content like 'madonna%' or content like 'nirvana%';`:
+-------------------------+
| title                   |
+-------------------------+
| Artist Profile: Madonna |
| Artist Profile: Nirvana |
+-------------------------+
2 results shown.
Search completed in 19 ms.
```

### GitHub Repositories & Branch Structure

hlquery is actively developed across multiple GitHub repositories. We maintain a structured development workflow to ensure stability and continuous improvement.

Each repository follows a **two-branch development model**:

- **`unstable`** - Active development branch where all new features, bug fixes, and improvements are developed
- **`1.0`** - Stable release branch containing production-ready code

### Active Development

We're committed to **active, continuous development** of hlquery and all related projects. New features, performance improvements, and bug fixes are regularly added across all repositories.

**Want to stay updated?** ⭐ **Star and watch our repositories on GitHub** to receive notifications about:
- New releases and features
- Bug fixes and improvements
- Documentation updates
- Community discussions

Subscribe to repository notifications to never miss an update!

### Contributing

We welcome contributions from the community! All contributions must be released under the BSD 3-Clause license.

### How to Contribute

- Check existing [issues](https://github.com/hlquery/hlquery/issues) or create new ones
- Contribute to client libraries (Node.js, TypeScript, Go, Java, Python, PHP, Ruby, Rust, Perl, C++)
- Test and report bugs
- Improve documentation

### Community

- 📖 [Documentation](https://docs.hlquery.com)
- 🐦 [X (Twitter)](https://x.com/hlquery)
- 📦 [GitHub](https://github.com/hlquery/hlquery)

### License

hlquery is licensed under the [BSD 3-Clause License](https://opensource.org/licenses/BSD-3-Clause).
