const { requestJson } = require("./http");

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function normalizeCollectionNames(raw) {
  if (!Array.isArray(raw)) {
    return [];
  }

  return raw
    .map((entry) => {
      if (typeof entry === "string") {
        return entry;
      }
      if (entry && typeof entry === "object" && typeof entry.name === "string") {
        return entry.name;
      }
      return "";
    })
    .filter(Boolean);
}

async function pMapLimit(items, limit, mapper) {
  const max = Math.max(1, limit | 0);
  const results = new Array(items.length);
  let nextIndex = 0;

  async function worker() {
    while (true) {
      const current = nextIndex++;
      if (current >= items.length) {
        return;
      }
      results[current] = await mapper(items[current], current);
    }
  }

  const workers = Array.from({ length: Math.min(max, items.length) }, () => worker());
  await Promise.all(workers);
  return results;
}

function shouldRetry(status, error) {
  if (error) {
    return true;
  }
  return status === 429 || status >= 500;
}

async function deleteWithRetry(name, opts) {
  const attempts = Math.max(1, (opts.retries | 0) + 1);
  let last = null;

  for (let i = 1; i <= attempts; i += 1) {
    const result = await requestJson({
      baseUrl: opts.baseUrl,
      method: "DELETE",
      path: `/collections/${encodeURIComponent(name)}`,
      token: opts.token,
      authMethod: opts.authMethod,
      timeoutMs: opts.timeoutMs,
    });

    last = { ...result, attempt: i };

    if (result.ok || !shouldRetry(result.status, result.error) || i === attempts) {
      return last;
    }

    await sleep(Math.min(4000, 200 * 2 ** (i - 1)));
  }

  return last;
}

async function listCollections(opts) {
  const response = await requestJson({
    baseUrl: opts.baseUrl,
    method: "GET",
    path: "/collections",
    token: opts.token,
    authMethod: opts.authMethod,
    timeoutMs: opts.timeoutMs,
  });

  if (!response.ok) {
    const reason = response.error || response.text || `HTTP ${response.status}`;
    throw new Error(`Failed to list collections: ${reason}`);
  }

  const body = response.data || {};
  return normalizeCollectionNames(body.collections || []);
}

function resolveTargets(allCollections, opts) {
  const explicit = Array.isArray(opts.names) ? opts.names.filter(Boolean) : [];
  const set = new Set(explicit);

  if (opts.prefix) {
    for (const name of allCollections) {
      if (name.startsWith(opts.prefix)) {
        set.add(name);
      }
    }
  }

  if (opts.regex) {
    const matcher = new RegExp(opts.regex);
    for (const name of allCollections) {
      if (matcher.test(name)) {
        set.add(name);
      }
    }
  }

  if (opts.all) {
    for (const name of allCollections) {
      set.add(name);
    }
  }

  return [...set].sort();
}

async function runDeleteJob(options) {
  const opts = {
    baseUrl: options.baseUrl || "http://127.0.0.1:9200",
    token: options.token || "",
    authMethod: options.authMethod === "api-key" ? "api-key" : "bearer",
    timeoutMs: Number.isFinite(options.timeoutMs) ? options.timeoutMs : 8000,
    retries: Number.isFinite(options.retries) ? options.retries : 3,
    concurrency: Number.isFinite(options.concurrency) ? options.concurrency : 8,
    dryRun: Boolean(options.dryRun),
    all: Boolean(options.all),
    prefix: options.prefix || "",
    regex: options.regex || "",
    names: Array.isArray(options.names) ? options.names : [],
  };

  const allCollections = await listCollections(opts);
  const targets = resolveTargets(allCollections, opts);

  if (targets.length === 0) {
    return {
      options: opts,
      discoveredCollections: allCollections.length,
      targeted: 0,
      deleted: 0,
      failed: 0,
      notFound: 0,
      dryRun: opts.dryRun,
      results: [],
    };
  }

  if (opts.dryRun) {
    return {
      options: opts,
      discoveredCollections: allCollections.length,
      targeted: targets.length,
      deleted: 0,
      failed: 0,
      notFound: 0,
      dryRun: true,
      results: targets.map((name) => ({
        name,
        status: "planned",
        httpStatus: 0,
        attempt: 0,
      })),
    };
  }

  const rawResults = await pMapLimit(targets, opts.concurrency, async (name) => {
    const result = await deleteWithRetry(name, opts);
    let status = "failed";

    if (result.ok) {
      status = "deleted";
    } else if (result.status === 404) {
      status = "not_found";
    }

    return {
      name,
      status,
      httpStatus: result.status,
      attempt: result.attempt || 1,
      message: result.error || (result.data && result.data.error) || result.text || "",
    };
  });

  const summary = {
    options: opts,
    discoveredCollections: allCollections.length,
    targeted: rawResults.length,
    deleted: rawResults.filter((r) => r.status === "deleted").length,
    failed: rawResults.filter((r) => r.status === "failed").length,
    notFound: rawResults.filter((r) => r.status === "not_found").length,
    dryRun: false,
    results: rawResults,
  };

  return summary;
}

module.exports = {
  runDeleteJob,
};
