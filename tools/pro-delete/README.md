# HLQuery Pro CLI Modules

Supporting modules used by `etc/ai/example.js`.

## Capabilities

- List collections
- List documents
- Get one document by ID
- Search documents
- Fetch/export many documents
- High-throughput collection delete (selectors + retries + concurrency)

## Entrypoint

Use from project root:

```bash
node etc/ai/example.js --help
```

## Examples

```bash
node etc/ai/example.js list collections
node etc/ai/example.js list docs --collection art --limit 50
node etc/ai/example.js get doc --collection art --id art_deep-dive-art-and-gallery
node etc/ai/example.js search docs --collection art --q gallery --query-by title,content
node etc/ai/example.js fetch docs --collection art --max 1000 --out art_dump.json
node etc/ai/example.js delete collections --prefix bench_ --dry-run --json
```
