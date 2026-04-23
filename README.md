<div align="center">
  <img src="https://docs.hlquery.com/img/hlquery/2.png" alt="hlquery logo" width="200">
</div>

<div align="center">

**A high-performance search engine built for modern applications.**

[![Follow hlquery](https://img.shields.io/badge/Follow-%40hlquery-blue?logo=x&logoColor=white)](https://x.com/hlquery)
[![Linux Build](https://github.com/hlquery/hlquery/workflows/Linux%20build/badge.svg)](https://github.com/hlquery/hlquery/actions)
[![macOS Build](https://github.com/hlquery/hlquery/workflows/macOS%20Build/badge.svg)](https://github.com/hlquery/hlquery/actions)
[![FreeBSD Build](https://github.com/hlquery/hlquery/workflows/FreeBSD%20Build/badge.svg)](https://github.com/hlquery/hlquery/actions)
[![Commit Activity](https://img.shields.io/github/commit-activity/m/hlquery/hlquery)](https://github.com/hlquery/hlquery/pulse)
[![hlquery](https://img.shields.io/badge/GitHub-hlquery-181717?logo=github&logoColor=white)](https://github.com/hlquery/hlquery/stargazers)
[![License](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

</div>

> You can explore the live demo at [demo.hlquery.com](https://demo.hlquery.com/).
> Demo mode is powered by [m_demo.cpp](https://github.com/hlquery/hlquery/blob/unstable/src/modules/m_demo.cpp), so insert operations are disabled.
> The demo UI is built with [hanalyzer](https://github.com/hlquery/hanalyzer).

### Overview

hlquery is an open source search engine written in C++17 and backed by RocksDB. It is designed for applications that need fast indexing, real-time queries, and a straightforward HTTP/JSON interface without giving up advanced search features. The engine supports full-text search, hybrid ranking, vector similarity, flexible collections, and configurable runtime modules for features such as AI-assisted search.
It exposes a REST API for indexing, querying, and administration, and includes command-line tools for local management and testing.

### Quick Start

Clone the repository normally:

```bash
$ git clone https://github.com/hlquery/hlquery.git
$ cd hlquery/
```

### Prerequisites

**Debian/Ubuntu:**
```bash
$ sudo apt-get install build-essential cmake libssl-dev
```

**RedHat/CentOS:**
```bash
$ dnf install @development-tools openssl-devel
```

**macOS:**
```bash
$ brew install openssl
```

**FreeBSD:**
```bash
$ sudo pkg install git gmake cmake openssl
```

> **Note**: This project uses GNU make features. On FreeBSD, run `gmake` instead of `make`.

### Installation

```bash
$ git clone --branch 1.0 https://github.com/hlquery/hlquery.git
$ cd hlquery/
$ ./configure
```

On Linux:

```
$ make -j10
$ make install
```

On FreeBSD, use GNU make for the build and install steps:

```bash
$ gmake -j10
$ gmake install
```

### Running hlquery

Start the server:

```bash
$ ./run/hlquery start
```

> **Note**: hlquery uses port **9200** by default. Ensure this port is available and not blocked by your firewall.

Stop the server:

```bash
$ ./run/hlquery stop
```

Run in foreground (for debugging):

```bash
$ ./run/hlquery start --nofork
```

Run talk:

```text
$ ./run/bin/hlquery-talk
localhost:9200> use art
Using collection 'art'.
localhost:9200|art> uptime
Server up for 3 days, 1h 0m 31s
```

## Client Libraries

Official client libraries are available for popular programming languages:

- **[Node.js](https://github.com/hlquery/node-api)** - Official Node.js client.
- **[Go](https://github.com/hlquery/go-api)** - Official Go client.
- **[Java](https://github.com/hlquery/java-api)** - Official Java client.
- **[Python](https://github.com/hlquery/python-api)** - Official Python client.
- **[PHP](https://github.com/hlquery/php-api)** - Official PHP client.
- **[Ruby](https://github.com/hlquery/ruby-api)** - Official Ruby client.
- **[Rust](https://github.com/hlquery/rust-api)** - Rust client library.
- **[Perl](https://github.com/hlquery/perl-api)** - Perl client library.
- **[C++](https://github.com/hlquery/cpp-api)** - C++ client library.

For complete API documentation, visit [docs.hlquery.com](https://docs.hlquery.com/).

## Getting Started

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

$client = new Client('http://localhost:9200');

$collections = $client->collections();

$schema = [
    'fields' => [
        ['name' => 'id', 'type' => 'string'],
        ['name' => 'title', 'type' => 'string'],
        ['name' => 'content', 'type' => 'string'],
        ['name' => 'price', 'type' => 'float'],
    ],
];

$collections->create('products', $schema);
```

### Index Documents

```bash
$ hlquery-cli add products product1 "Laptop Computer" "High-performance laptop with 16GB RAM"
Document 'product1' added to collection 'products'
```

**Using the Node API:**

```js
await client.documents().add('products', {
  id: 'product1',
  title: 'Laptop Computer',
  content: 'High-performance laptop with 16GB RAM'
});
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
hlquery::Client client("http://localhost:9200");
auto result = client.search("products", {{"q", "laptop"}});
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

## GitHub Repositories & Development Workflow

hlquery is actively developed across multiple GitHub repositories. We maintain a structured development workflow to ensure stability and continuous improvement.

### Branch Structure

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

## Contributing

We welcome contributions from the community! All contributions must be released under the BSD 3-Clause license.

### How to Contribute

- Check existing [issues](https://github.com/hlquery/hlquery/issues) or create new ones
- Contribute to client libraries (Node.js, Go, Java, Python, PHP, Ruby, Rust, Perl, C++)
- Test and report bugs
- Improve documentation
- Join our [Discord community](https://discord.hlquery.com)

## Community

- 📖 [Documentation](https://docs.hlquery.com)
- 💬 [Discord](https://discord.hlquery.com)
- 🐦 [X (Twitter)](https://x.com/hlquery)
- 📦 [GitHub](https://github.com/hlquery/hlquery)

## License

hlquery is licensed under the [BSD 3-Clause License](https://opensource.org/licenses/BSD-3-Clause).
