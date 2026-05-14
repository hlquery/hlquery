const fs = require("fs");
const path = require("path");
const { requestJson } = require("./http");

function firstArray(body, keys) {
  for (const key of keys) {
    if (body && Array.isArray(body[key])) {
      return body[key];
    }
  }
  return [];
}

function normalizeHitArray(body) {
  if (!body || typeof body !== "object") {
    return [];
  }

  if (Array.isArray(body.hits)) {
    return body.hits.map((hit) => {
      if (hit && typeof hit === "object" && hit.document && typeof hit.document === "object") {
        return hit.document;
      }
      return hit;
    });
  }

  return firstArray(body, ["documents", "docs", "results"]);
}

async function listCollections(opts) {
  return requestJson({
    baseUrl: opts.baseUrl,
    method: "GET",
    path: `/collections?offset=${encodeURIComponent(opts.offset)}&limit=${encodeURIComponent(opts.limit)}`,
    token: opts.token,
    authMethod: opts.authMethod,
    timeoutMs: opts.timeoutMs,
  });
}

async function listDocs(opts) {
  return requestJson({
    baseUrl: opts.baseUrl,
    method: "GET",
    path: `/collections/${encodeURIComponent(opts.collection)}/documents?offset=${encodeURIComponent(opts.offset)}&limit=${encodeURIComponent(opts.limit)}`,
    token: opts.token,
    authMethod: opts.authMethod,
    timeoutMs: opts.timeoutMs,
  });
}

async function getDoc(opts) {
  return requestJson({
    baseUrl: opts.baseUrl,
    method: "GET",
    path: `/collections/${encodeURIComponent(opts.collection)}/documents/${encodeURIComponent(opts.id)}`,
    token: opts.token,
    authMethod: opts.authMethod,
    timeoutMs: opts.timeoutMs,
  });
}

async function searchDocs(opts) {
  const params = new URLSearchParams();
  params.set("q", opts.q || "*");
  params.set("offset", String(opts.offset));
  params.set("limit", String(opts.limit));
  if (opts.queryBy) {
    params.set("query_by", opts.queryBy);
  }
  if (opts.filterBy) {
    params.set("filter_by", opts.filterBy);
  }
  if (opts.sortBy) {
    params.set("sort_by", opts.sortBy);
  }

  return requestJson({
    baseUrl: opts.baseUrl,
    method: "GET",
    path: `/collections/${encodeURIComponent(opts.collection)}/documents/search?${params.toString()}`,
    token: opts.token,
    authMethod: opts.authMethod,
    timeoutMs: opts.timeoutMs,
  });
}

async function fetchDocs(opts) {
  const pageSize = Math.max(1, opts.limit | 0);
  const maxItems = Math.max(1, opts.max | 0);
  let offset = Math.max(0, opts.offset | 0);
  const out = [];

  while (out.length < maxItems) {
    const response = await listDocs({
      ...opts,
      offset,
      limit: Math.min(pageSize, maxItems - out.length),
    });

    if (!response.ok) {
      return {
        ok: false,
        status: response.status,
        error: response.error || response.text || `HTTP ${response.status}`,
        data: {
          fetched: out.length,
          documents: out,
        },
      };
    }

    const docs = normalizeHitArray(response.data || {});
    if (docs.length === 0) {
      break;
    }

    out.push(...docs);
    offset += docs.length;

    if (docs.length < Math.min(pageSize, maxItems - out.length + docs.length)) {
      break;
    }
  }

  if (opts.outFile) {
    const filePath = path.resolve(process.cwd(), opts.outFile);
    fs.mkdirSync(path.dirname(filePath), { recursive: true });
    fs.writeFileSync(filePath, JSON.stringify({ collection: opts.collection, count: out.length, documents: out }, null, 2));
  }

  return {
    ok: true,
    status: 200,
    data: {
      collection: opts.collection,
      count: out.length,
      documents: out,
      outFile: opts.outFile || "",
    },
    text: "",
  };
}

module.exports = {
  listCollections,
  listDocs,
  getDoc,
  searchDocs,
  fetchDocs,
  normalizeHitArray,
};
