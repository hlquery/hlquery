> **Development Status**: hlquery is currently in active development and should not be used in production environments. The software may contain bugs, incomplete features, and breaking changes may occur without notice.

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
> Demo mode is powered by [m_demo.cpp](https://github.com/hlquery/hlquery/blob/unstable/src/modules/m_demo.cpp), so insert, delete, and update operations are disabled.
> The demo UI is built with [hanalyzer](https://github.com/hlquery/hanalyzer).

### What is hlquery?

hlquery is an open source search engine written in C++, optimized to stay lightweight while handling millions of results efficiently. It is designed for applications that need fast indexing, real-time queries, and a straightforward HTTP/JSON interface without giving up advanced search features. The engine supports full-text search, hybrid ranking, vector similarity, flexible collections, and configurable runtime modules for features such as AI-assisted search.
It exposes a REST API for indexing, querying, and administration, and includes command-line tools for local management and testing.

### Why choose hlquery?

hlquery is built for teams that want strong search capabilities without taking on the operational weight of a larger search stack. It combines fast indexing, low-latency queries, and a simple HTTP/JSON surface area with features that are usually expected from more complex systems.

You can use hlquery for classic full-text search, hybrid retrieval, vector similarity, and AI-assisted search workflows while keeping deployment and integration straightforward. Its optional SAM+ layer can add adaptive, assistant-driven search behavior when you need it. The project also ships with official client libraries, command-line tools, and modular runtime extensions, which makes it practical both for local development and production services.

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

**Note**: This project uses gmake features. On FreeBSD, run `gmake` instead of `make`.

### Installation

```bash
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
$ ./run/hlquery talk
localhost:9200> use art
Using collection 'art'.
localhost:9200|art> uptime
Server up for 3 days, 1h 0m 31s
```

### Client Libraries

Official client libraries are available for popular programming languages:

| Client | Description |
| --- | --- |
| **[C++](https://github.com/hlquery/cpp-api)** | Native C++ client library for low-level and embedded integrations. |
| **[Go](https://github.com/hlquery/go-api)** | Idiomatic Go client for indexing, search, and service backends. |
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

$collections = $client->collections();

/* Build the schema payload sent to hlquery. */

$schema = [
    'fields' => [
        /* Keep the main product title searchable.     */

        ['name' => 'title', 'type' => 'string'],

        /* Index the longer product description text.  */

        ['name' => 'content', 'type' => 'string'],

        /* Save a numeric price for filters and sorts. */

        ['name' => 'price', 'type' => 'float'],
    ],
];

$response = $collections->create('products', $schema); 
$body = $response->getBody(); 

$client->documents()->add('products', [
    'id' => 'prod_keyboard_001',
    'title' => 'Wireless Keyboard',
    'content' => 'Compact Bluetooth keyboard for daily work.',
    'price' => 49.99,
]);

```

### Index Documents

Each document id must be unique within its collection:

```bash
$ hlquery-cli add products prod_laptop_001 "Laptop Computer" "High-performance laptop with 16GB RAM"
Document 'prod_laptop_001' added to collection 'products'
```

**Using the Node API:**

```js
const Client = require('hlquery-node-client');
const client = new Client('http://localhost:9200'); 
const documents = client.documents(); // Use the documents service for document writes.

/* Send POST /collections/products/documents with the product payload. */

const response = await documents.add('products', {
  id: 'prod_laptop_001', // Unique document id used by later reads and updates.
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

+---+-----------------+----------+-----------------+---------------------------------------+
| # | Document ID     | Score    | Title           | Content Preview                       |
+---+-----------------+----------+-----------------+---------------------------------------+
| 1 | prod_laptop_001 | 1.094500 | Laptop Computer | High-performance laptop with 16GB RAM |
+---+-----------------+----------+-----------------+---------------------------------------+
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

### SAM+ (optional): Search that gets smarter over time

hlquery includes **SAM+**, the **Secondary Assistant Manager**, as a 100% optional feature. SAM+ adds a second retrieval layer for natural-language intent, search assistance, and learned query behavior, but hlquery can run normally without it.

To use SAM+, enable it in [run/conf/sam.conf](run/conf/sam.conf) and point it at a local Qwen model. The main [run/conf/hlquery.conf](run/conf/hlquery.conf) includes `sam.conf` during startup:

```xml
<sam enabled="true"
     models_dir="run/models"
     model_name="qwen_1_5">
```

Download the matching model with the bundled helper:

```bash
$ ./tools/download --model qwen_1_5
```

`tools/download` stores model files under `run/models` by default. The shipped Qwen presets include `qwen_0_5`, `qwen_1_5`, `qwen_3`, `qwen_14`, and `qwen_coder_1_5`; the `model_name` in `sam.conf` should match the preset you download.

The GGUF file under `run/models` is model data, so SAM+ also needs a local inference runtime. To fetch and build a repo-local `llama.cpp` runtime:

```bash
$ make llama-runtime-fetch
$ make llama-runtime-check
```

SAM+ prefers that vendored runtime automatically. It starts a process for each inference request and defaults to two CPU threads so background work does not starve HTTP handling. Set `HLQUERY_LLAMA_THREADS` to tune that limit. Use `qwen_1_5` or `qwen_0_5` for low-latency local development; `qwen_14` reloads a substantially larger model each time.

---

**SQL example:**

```text
$ ./run/hlquery talk
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
