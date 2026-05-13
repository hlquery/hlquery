const DEFAULT_TIMEOUT_MS = 8000;

function buildHeaders(token, authMethod, hasBody) {
  const headers = {
    Accept: "application/json",
  };

  if (hasBody) {
    headers["Content-Type"] = "application/json";
  }

  if (!token) {
    return headers;
  }

  if (authMethod === "api-key") {
    headers["X-API-Key"] = token;
  } else {
    headers.Authorization = `Bearer ${token}`;
  }

  return headers;
}

async function requestJson({
  baseUrl,
  path,
  method = "GET",
  token = "",
  authMethod = "bearer",
  body = null,
  timeoutMs = DEFAULT_TIMEOUT_MS,
}) {
  const url = `${baseUrl.replace(/\/+$/, "")}${path}`;
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), timeoutMs);

  try {
    const response = await fetch(url, {
      method,
      headers: buildHeaders(token, authMethod, Boolean(body)),
      body: body ? JSON.stringify(body) : undefined,
      signal: controller.signal,
    });

    const text = await response.text();
    let data = null;
    try {
      data = text ? JSON.parse(text) : null;
    } catch (_) {
      data = null;
    }

    return {
      ok: response.ok,
      status: response.status,
      data,
      text,
    };
  } catch (error) {
    return {
      ok: false,
      status: 0,
      data: null,
      text: "",
      error: error && error.message ? error.message : "Network error",
    };
  } finally {
    clearTimeout(timeout);
  }
}

module.exports = {
  requestJson,
};
