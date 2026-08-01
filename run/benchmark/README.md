# Synthetic benchmark fixtures

These files power `hlquery-benchmark --fake`. They contain 630 deterministic
demo records across 19 collections.

They are separate from the high-volume `bench_*` collections. The normal
benchmark document generator, naming, payload shape, and performance workload
are unchanged by these public demo fixtures.

People, organizations other than the named universities, artworks, companies,
incidents, and market instruments in these fixtures are fictional. University
names, cities, states, countries, and broad institution types are catalog
references; their `search_topics` are synthetic query aids. The fixture contains
no university ranking, score, enrollment count, or invented campus coordinate.
Finance and stock scenarios contain no live data or recommendations.

Every imported record includes:

- `is_synthetic: true`
- a human-readable `data_notice`
- a deterministic four-dimensional `embedding`
- a `location_name`; wholly synthetic collections also have a demo `location`

Collection metadata also includes `_demo_description` and `_demo_queries`, so
public interfaces can explain what each dataset demonstrates and offer useful
queries instead of exposing filler text. For example:

- `people`: `science educator California`, `civil engineer sustainable materials`
- `universities`: `science universities`, `public research universities in Texas`
- `art`: `recycled steel sculpture`, `charcoal portrait`

University IDs are derived from institution names, so collection listings show
useful references such as `universities_harvard-university` instead of numeric
placeholder IDs. `catalog_order` provides deterministic alphabetical sorting.

Edit the source definitions in
`tools/generate-benchmark-fixtures.py`, then regenerate the JSON:

```bash
./tools/generate-benchmark-fixtures.py
```

The generator is deterministic, so running it without source changes should not
change any fixture. It also validates every public document before writing:
synthetic labels and notices are mandatory, fictional people and organizations
must retain their explicit markers, contact details are forbidden, university
IDs and catalog order must be meaningful, fabricated university rankings and
enrollment counts are forbidden, and finance records retain their disclaimer.

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
