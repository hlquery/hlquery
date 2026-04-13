<div align="center">
  <img src="https://docs.hlquery.com/img/hlquery/2.png" alt="hlquery logo" width="200">
</div>

<div align="center">

**A high-performance search engine built for modern applications**

[![Twitter Follow](https://img.shields.io/twitter/url/https/x.com/hlquery.svg?style=social&label=Follow%20%40hlquery)](https://x.com/hlquery)
[![Linux Build](https://github.com/hlquery/hlquery/workflows/Linux%20build/badge.svg)](https://github.com/hlquery/hlquery/actions)
[![macOS Build](https://github.com/hlquery/hlquery/workflows/macOS%20Build/badge.svg)](https://github.com/hlquery/hlquery/actions)
[![Commit Activity](https://img.shields.io/github/commit-activity/m/hlquery/hlquery)](https://github.com/hlquery/hlquery/pulse)
[![GitHub stars](https://img.shields.io/github/stars/hlquery/hlquery?style=social)](https://github.com/hlquery/hlquery/stargazers)
[![License](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

[Documentation](https://docs.hlquery.com) • [GitHub](https://github.com/hlquery/hlquery) • [Discord](https://discord.hlquery.com)

</div>

---

> **Development Status**: hlquery is currently in active development and should not be used in production environments. The software may contain bugs, incomplete features, and breaking changes may occur without notice.

## Overview

hlquery is an open source search engine written in C++17 and backed by RocksDB. It is designed for applications that need fast indexing, real-time queries, and a straightforward HTTP/JSON interface without giving up advanced search features. The engine supports full-text search, hybrid ranking, vector similarity, flexible collections, and configurable runtime modules for features such as AI-assisted search.
It exposes a REST API for indexing, querying, and administration, and includes command-line tools for local management and testing.

## Quick Start

Clone the repository normally:

```bash
$ git clone https://github.com/hlquery/hlquery.git
$ cd hlquery
```

### Prerequisites

**Debian/Ubuntu:**
```bash
$ sudo apt-get install build-essential cmake
```

**RedHat/CentOS:**
```bash
$ dnf install @development-tools
```

**macOS:**
No additional dependencies required.

> **Note**: For SSL/TLS support, install `libssl-dev` (Debian), `openssl-devel` (RedHat), or `openssl` (macOS).

### Installation

```bash
$ git clone --branch 1.0 https://github.com/hlquery/hlquery.git
$ cd hlquery/
$ ./configure
$ make -j$(nproc)
$ make install
```

### Running hlquery

Start the server:
```bash
$ ./run/hlquery start
```

Stop the server:
```bash
$ ./run/hlquery stop
```

Check server status:
```bash
$ ./run/hlquery status
```

Run in foreground (for debugging):
```bash
$ ./run/hlquery start --nofork
```

> **Note**: hlquery uses port **9200** by default. Ensure this port is available and not blocked by your firewall.

## Getting Started

### Create a Collection

```bash
$ hlquery-cli create products title content price
Collection 'products' created successfully
```

### Index Documents

```bash
$ hlquery-cli add products product1 "Laptop Computer" "High-performance laptop with 16GB RAM"
Document 'product1' added to collection 'products'
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

# Boost term importance
$ hlquery-cli search products "laptop^2.0 computer"

# NOT operator
$ hlquery-cli search products "!apple"

# Combined queries
$ hlquery-cli search products "title:laptop AND price:[100 TO 500]"
```

## Client Libraries

Official client libraries are available for popular programming languages:

- **[Node.js](https://github.com/hlquery/node-api)** - Official Node.js client
- **[Python](https://github.com/hlquery/python-hlquery)** - Official Python client
- **[PHP](https://github.com/hlquery/php-api)** - Official PHP client
- **[Rust](https://github.com/hlquery/rust-api)** - Rust client library
- **[Perl](https://github.com/hlquery/perl-api)** - Perl client library
- **[C++](https://github.com/hlquery/cpp-api)** - C++ client library

For complete API documentation, visit [docs.hlquery.com](https://docs.hlquery.com/).

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
- Contribute to client libraries (Node.js, Python, PHP, Rust, Perl, C++)
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
