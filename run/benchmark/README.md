# Synthetic benchmark fixtures

These files power `hlquery-benchmark --fake`. They contain 630 deterministic
demo records across 19 collections.

They are separate from the high-volume `bench_*` collections. The normal
benchmark document generator, naming, payload shape, and performance workload
are unchanged by these public demo fixtures.

People, organizations other than the named universities, artworks, companies,
incidents, and market instruments in these fixtures are fictional. The
`universities` fixture is a dated snapshot of the top 100 entries in the
Webometrics January 2026 world ranking. Its names, locations, published
ranking fields, edition, and source URL are factual references; `search_topics`
and descriptive prose are synthetic query aids. City and metropolitan-area
labels make nearby campuses discoverable by the city users normally search—for
example, `Boston` matches MIT in Cambridge as well as Boston University. It
contains no enrollment count or invented campus coordinate. Finance and stock
scenarios contain no live data or recommendations.

Every imported record includes:

- `is_synthetic: true` when any generated benchmark content is present
- a human-readable `data_notice`
- a deterministic four-dimensional `embedding`
- a `location_name`; wholly synthetic collections also have a demo `location`

Collection metadata also includes `_demo_description` and `_demo_queries`, so
public interfaces can explain what each dataset demonstrates and offer useful
queries instead of exposing filler text. For example:

- `people`: `science educator California`, `senior public health analyst Texas`
- `universities`: `Boston universities`, `universities in London`
- `art`: `recycled steel sculpture`, `charcoal portrait`
- `science`: `controlled battery discharge test`, `noisy sensor data limitations`

## Credibility verification

The ordinary 100,000-document command is a smoke test. It performs a checked
multi-instance WAL barrier and complete deterministic SHA-256 read-back, but it
must not be presented as sustained production throughput.

```bash
./scripts/benchmark/verify-credibility.sh \
  --server ./run/bin/hlquery \
  --benchmark ./run/bin/hlquery-benchmark \
  --config ./run/conf/hlquery.conf \
  --profile standard \
  --output ./artifacts/benchmark
```

Profiles are `smoke`, `standard`, `sustained`, and `out-of-cache`. Standard
automatically calibrates to at least 60 seconds and retains ten measured runs;
sustained calibrates to at least ten minutes. `--matrix core` sweeps each local
worker, batch, and collection axis; `--matrix full` runs their cross-product.
The syscall, process-crash, and VM-power-loss tools under `scripts/benchmark/`
produce separate claims; process-crash success is never labeled as a VM or
physical power-loss guarantee.

University IDs are derived from institution names, so collection listings show
useful references such as `universities_harvard-university` instead of numeric
placeholder IDs. `webometrics_world_rank` provides deterministic ranking order;
`catalog_order` mirrors that rank for compatibility.

Edit the source definitions in
`tools/generate-benchmark-fixtures.py`, then regenerate the JSON:

```bash
./tools/generate-benchmark-fixtures.py
```

The generator is deterministic, so running it without source changes should not
change any fixture. It also validates every public document before writing:
synthetic labels and notices are mandatory, fictional people and organizations
must retain their explicit markers, contact details are forbidden, university
IDs and catalog order must be meaningful, sourced Webometrics fields must be
complete, unsourced rankings and enrollment counts are forbidden, and finance
records retain their disclaimer.

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
