# Benchmark fixtures

`hlquery-benchmark --fake` loads every `.json` file in this directory in
lexicographic order. One regular file defines one collection. Files whose name
starts with `_` define global synonyms and stopwords.

The directory can be overridden with `HLQUERY_BENCHMARK_DIR`.

A collection fixture accepts:

- `collection`: required collection name.
- `count`: number of generated documents.
- `tags`, `title_template`, `content_template`: generated document data.
- `documents`: optional explicit document objects (cycled up to `count`).
- `fields`, `default_sorting_field`, `metadata`: collection schema.
- `document_defaults`: fields added to every document.
- `sequence_fields`: numeric fields with `start`, `step`, and optional
  `integer`.
- `synonyms`, `stopwords`, `aliases`: collection resources.

Templates support `{collection}`, `{tag}`, and `{index}`.
