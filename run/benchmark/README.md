# Synthetic benchmark fixtures

These files power `hlquery-benchmark --fake`. They contain 620 deterministic
demo records across 19 collections.

All people, organizations, artworks, companies, incidents, market instruments,
and university rankings in these fixtures are fictional. Real place names are
used only as broad geographic search examples. The university `demo_rank` and
component scores are generated for sorting and ranking demonstrations; they do
not evaluate real institutions or reproduce a third-party ranking. Finance and
stock scenarios contain no live data or recommendations.

Every imported record includes:

- `is_synthetic: true`
- a human-readable `data_notice`
- a deterministic four-dimensional `embedding`
- a demo `location` and `location_name`

Edit the source definitions in
`tools/generate-benchmark-fixtures.py`, then regenerate the JSON:

```bash
./tools/generate-benchmark-fixtures.py
```

The generator is deterministic, so running it without source changes should not
change any fixture.

`hlquery-benchmark --fake` loads every `.json` file in this directory in
lexicographic order. Files whose name starts with `_` define global synonyms and
stopwords. Override the directory with `HLQUERY_BENCHMARK_DIR`.

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
