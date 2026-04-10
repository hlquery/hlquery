<div align="center">
  <img src="https://docs.hlquery.com/img/hlquery/2.png" alt="hlquery logo" width="200">
</div>

<div align="center">

# hlquery

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

hlquery is a RESTful search engine built with C++17 and powered by RocksDB. It provides real-time search capabilities with advanced features including full-text search, hybrid search (combining keyword and semantic matching), vector similarity search, and intelligent ranking algorithms that adapt automatically for optimal relevance.

### Key Features

- **Full-Text Search**: Advanced BM25+ ranking with automatic parameter optimization
- **Hybrid Search**: Seamlessly combines keyword and semantic matching for superior results
- **Vector Search**: High-performance similarity search for embeddings and semantic queries
- **Adaptive Ranking**: Algorithms automatically adjust parameters for optimal relevance
- **Collections & Documents**: Flexible schema design with support for multiple data types
- **Advanced Query Syntax**: Field queries, ranges, wildcards, fuzzy matching, regex, and boosting
- **Synonyms & Stopwords**: Built-in support for language-specific text processing
- **RESTful API**: Simple HTTP/JSON interface for easy integration
- **Multi-Platform**: Available for GNU/Linux, FreeBSD, and macOS

## Quick Start

Clone the repository normally:

```bash
git clone https://github.com/hlquery/hlquery.git
cd hlquery
```

### Prerequisites

**Debian/Ubuntu:**
```bash
sudo apt-get install build-essential cmake
```

**RedHat/CentOS:**
```bash
dnf install @development-tools
```

**macOS:**
No additional dependencies required.

> **Note**: For SSL/TLS support, install `libssl-dev` (Debian), `openssl-devel` (RedHat), or `openssl` (macOS).

### Installation

```bash
# For production use (stable branch)
git clone --branch 1.0 https://github.com/hlquery/hlquery.git
cd hlquery/
./configure
make -j$(nproc)
sudo make install
```

> **Note**: For development or to try the latest features, use the `unstable` branch:
> ```bash
> git clone --branch unstable https://github.com/hlquery/hlquery.git
> ```

> **Tip**: Adjust the `-j` flag based on your CPU core count for optimal build performance.

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

```bash
$ hlquery-cli search products "laptop"
Search results for 'laptop' in collection 'products':
Found 1 document(s) (showing 1-1 of 1)

+---+-------------+----------+-----------------+---------------------------------------+----------+
| # | Document ID | Score    | Title           | Content Preview                       | Fields   |
+---+-------------+----------+-----------------+---------------------------------------+----------+
| 1 | product1    | 1.094500 | Laptop Computer | High-performance laptop with 16GB RAM | 0 fields |
+---+-------------+----------+-----------------+---------------------------------------+----------+
```

### Natural-Language AI Search

The `m_ai_search` runtime module lets you execute human-friendly product queries such as "shoes for wedding" or "formal evening bag" by scoring the `title`, `description`, and `labels` fields for every document. Labels can be stored as JSON arrays or comma-separated strings, and the module still works when those fields are missing by falling back to document content.

The module exposes `/modules/ai_search/search` over HTTP and can be configured through the `<ai_search ...>` block in `run/conf/modules.conf`. Raise `label_phrase_weight`/`label_exact_token_weight` to privilege labeled hits, or tweak the description weights if you care more about richer text.

Note: modules that depend on LLM inference declare the requirement flag in code so hlquery refuses to load them unless the shared `<llm>` block is configured with a model.

Example HTTP query:

```bash
curl "http://localhost:9200/modules/ai_search/search?collection=products&q=shoes%20for%20wedding&limit=5"
```

CLI example (positional terms are used when `q=` is omitted):

```bash
xx module ai_search ecommerce search "formal evening bag"
```

You can omit `collection` to scan everything, or supply comma-separated names via `collection=electronics,apparel`. The module returns the `matched_on` fields and a `description_snippet` to help you inspect why each hit scored highly.

## Query Syntax

hlquery supports a rich query syntax for powerful search capabilities:

| Feature | Syntax | Example |
|---------|--------|---------|
| **Field Query** | `field:value` | `title:laptop` |
| **Range Query** | `field:[min TO max]` | `price:[100 TO 500]` |
| **Wildcard** | `term*`, `*term` | `laptop*`, `*laptop` |
| **Fuzzy Search** | `term~` or `term~2` | `laptop~2` |
| **Regex** | `field:/pattern/` | `title:/laptop.*computer/` |
| **Boost** | `term^weight` | `laptop^2.0` |
| **NOT** | `!term` or `NOT term` | `!apple` |
| **Boolean** | `AND`, `OR`, `NOT` | `title:laptop AND price:[100 TO 500]` |

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

## LLM Model Helpers

Use the bundled downloader in `tools/download` (which can also be executed as `perl tools/download`) to fetch GGUF artifacts into `run/models`. The script knows several presets (run `tools/download --list-names`) and accepts `--byname`, `--repo`, `--file`, or `--url` to point at a different Hugging Face repo or direct link. Override `--dir`/`--out` to target custom paths, and pass `--force` to re-download an already existing file.

```bash
tools/download --byname qwen_latest
tools/download --repo TheBloke/Baichuan2-7B-Chat-GGUF --file baichuan2-7b-chat.Q4_K_M.gguf
```

Paths are resolved relative to the config directory, so `models_dir="run/models"` just needs to exist once you've downloaded the weights.

## AI Search Smoke Test

The PHP smoke test under `tests/llm.php` spins up an `ecommerce` collection, inserts sample documents with labels/descriptions, and verifies that `m_ai_search` returns the expected hits for queries like \"wedding shoes\" and \"formal evening bag\". Run it against a running server at `http://localhost:9200`:

```bash
php tests/llm.php
```

## GitHub Repositories & Development Workflow

hlquery is actively developed across multiple GitHub repositories. We maintain a structured development workflow to ensure stability and continuous improvement.

### Our Repositories

- **[hlquery](https://github.com/hlquery/hlquery)** - Main search engine (C++, BSD 3-Clause License)
- **[rapid](https://github.com/hlquery/rapid)** - Quick deployment tool (TypeScript)
- **[cpp-api](https://github.com/hlquery/cpp-api)** - C++ client library
- **[hanalyzer](https://github.com/hlquery/hanalyzer)** - Web interface and management UI (Vue 3)
- **[package-builder](https://github.com/hlquery/package-builder)** - Packaging scripts for distributions
- **[docker](https://github.com/hlquery/docker)** - Docker configurations and containerization
- **[rust-api](https://github.com/hlquery/rust-api)** - Rust client library
- **[php-api](https://github.com/hlquery/php-api)** - PHP client library
- **[perl-api](https://github.com/hlquery/perl-api)** - Perl client library
- **[node-api](https://github.com/hlquery/node-api)** - Node.js/TypeScript client library

### Branch Structure

Each repository follows a **two-branch development model**:

- **`unstable`** - Active development branch where all new features, bug fixes, and improvements are developed
- **`1.0`** - Stable release branch containing production-ready code

### Development Workflow

1. **Development happens on `unstable`** - All commits, features, and fixes are made to the `unstable` branch
2. **Testing and validation** - Code is thoroughly tested on `unstable` before merging
3. **Merge to `1.0`** - Once stable and validated, changes are merged from `unstable` to `1.0`
4. **Production use** - The `1.0` branch is recommended for production deployments

### Active Development

We're committed to **active, continuous development** of hlquery and all related projects. New features, performance improvements, and bug fixes are regularly added across all repositories.

**Want to stay updated?** ⭐ **Star and watch our repositories on GitHub** to receive notifications about:
- New releases and features
- Bug fixes and improvements
- Documentation updates
- Community discussions

Subscribe to repository notifications to never miss an update!

## Architecture

hlquery is built with a modular architecture:

- **`src/core/`** - Core server components, event loop, and threading
- **`src/api/`** - HTTP server, REST endpoints, and API handlers
- **`src/search/`** - RocksDB-backed search storage integration
- **`include/`** - Header files and public APIs
- **`vendor/`** - Third-party dependencies

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

---

<div align="center">

⭐ **Star us on GitHub if you find hlquery useful!**

Made with ❤️ by the hlquery team

</div>
