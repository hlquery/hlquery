# Tests

This directory contains small root-level validation scripts for the `hlquery`
workspace.

The PHP runner validates repository configuration and exercises the daemon's
health, status, and collections endpoints. Set `HLQUERY_URL` for a non-default
endpoint and `HLQUERY_API_KEY` when authentication is enabled.

- `tests/php/run_all.php`

Run it with:

```bash
php tests/php/run_all.php
```
