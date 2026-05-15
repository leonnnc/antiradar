const JSON_HEADERS = {
  "content-type": "application/json; charset=utf-8",
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "GET, OPTIONS",
  "access-control-allow-headers": "content-type"
};

function json(body, status = 200) {
  return new Response(JSON.stringify(body, null, 2), {
    status,
    headers: JSON_HEADERS
  });
}

function cleanUsername(value) {
  const username = String(value || "").trim().replace(/^@/, "").toLowerCase();
  if (!/^[a-z0-9._]{1,30}$/.test(username)) return "";
  return username;
}

async function fetchInstagramStats(username, env) {
  const token = env.META_ACCESS_TOKEN;
  const igUserId = env.IG_USER_ID;
  const version = env.GRAPH_API_VERSION || "v22.0";

  if (!token || !igUserId) {
    return {
      ok: false,
      status: 500,
      body: {
        status: "error",
        error: "backend_not_configured",
        message: "Faltan secretos META_ACCESS_TOKEN o IG_USER_ID"
      }
    };
  }

  const fields = `business_discovery.username(${username}){username,followers_count,media_count}`;
  const graphUrl =
    `https://graph.facebook.com/${version}/${encodeURIComponent(igUserId)}` +
    `?fields=${encodeURIComponent(fields)}` +
    `&access_token=${encodeURIComponent(token)}`;

  const response = await fetch(graphUrl, {
    headers: {
      "accept": "application/json"
    }
  });
  const payload = await response.json().catch(() => ({}));

  if (!response.ok || payload.error) {
    return {
      ok: false,
      status: response.status || 502,
      body: {
        status: "error",
        error: payload.error?.code ? `meta_${payload.error.code}` : "meta_error",
        message: payload.error?.message || "Meta Graph API no devolvio datos"
      }
    };
  }

  const discovered = payload.business_discovery || {};
  if (typeof discovered.followers_count !== "number") {
    return {
      ok: false,
      status: 404,
      body: {
        status: "error",
        error: "no_followers_count",
        message: "La cuenta debe ser publica y Business/Creator para Business Discovery"
      }
    };
  }

  return {
    ok: true,
    status: 200,
    body: {
      status: "ok",
      username: discovered.username || username,
      followers: discovered.followers_count,
      followers_count: discovered.followers_count,
      media: discovered.media_count || 0,
      media_count: discovered.media_count || 0,
      source: "meta_graph_business_discovery",
      updated_at: new Date().toISOString()
    }
  };
}

export default {
  async fetch(request, env) {
    if (request.method === "OPTIONS") {
      return new Response(null, { headers: JSON_HEADERS });
    }

    const url = new URL(request.url);
    if (url.pathname === "/" || url.pathname === "/health") {
      return json({
        status: "ok",
        service: "CYBERDECK Insta Monitor API",
        usage: "/ig?user=pepeangelll"
      });
    }

    if (url.pathname !== "/ig") {
      return json({ status: "error", error: "not_found" }, 404);
    }

    const username = cleanUsername(url.searchParams.get("user"));
    if (!username) {
      return json({
        status: "error",
        error: "invalid_username",
        message: "Usa /ig?user=usuario sin espacios ni caracteres raros"
      }, 400);
    }

    const result = await fetchInstagramStats(username, env);
    return json(result.body, result.status);
  }
};
