/**
 * Cloudflare Worker: RDM-7 OTA Firmware Proxy + CAN Log Upload Endpoint
 *
 * Routes:
 *   GET  /                  — health check
 *   GET  /<version>/<file>.bin — resolve GitHub release download → CDN URL
 *   POST /can-upload        — accept a CAN trace from a dashboard
 *                             (HMAC-SHA256 authenticated, stored in R2)
 *
 * Bindings:
 *   env.CAN_LOGS                — R2 bucket where traces land
 *   env.CAN_UPLOAD_HMAC_SECRET  — wrangler secret; same value baked into
 *                                 firmware at main/include/can_upload_secret.h
 *
 * URL pattern (OTA): https://<worker>/<version>/esp32-firmware.bin
 * GitHub source: Trizzoto/potato-jubilee (public repo, releases)
 */

const GITHUB_OWNER = "Trizzoto";
const GITHUB_REPO = "potato-jubilee";
const CAN_UPLOAD_MAX_BYTES = 10 * 1024 * 1024;  // 10 MB per trace
const HMAC_TIMESTAMP_WINDOW_S = 600;             // ±10 min

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;

    // Health check
    if (path === "/" || path === "/health") {
      return jsonResponse({ status: "ok", service: "rdm7-ota-proxy" });
    }

    // CAN log upload
    if (path === "/can-upload") {
      if (request.method !== "POST") {
        return new Response("POST only", { status: 405 });
      }
      return handleCanUpload(request, env);
    }

    // CAN log listing — requires the same HMAC secret as upload, passed as a
    // query param `?secret=...` so it's curl-friendly. Returns the 100 most
    // recent objects with their metadata.
    if (path === "/can-list") {
      if (!env.CAN_LOGS) return jsonResponse({ error: "R2 binding missing" }, 500);
      const secret = url.searchParams.get("secret") || "";
      if (!env.CAN_UPLOAD_HMAC_SECRET || secret !== env.CAN_UPLOAD_HMAC_SECRET) {
        return jsonResponse({ error: "Unauthorized" }, 401);
      }
      const prefix = url.searchParams.get("prefix") || "";
      const listed = await env.CAN_LOGS.list({
        prefix, limit: 100, include: ["customMetadata", "httpMetadata"],
      });
      const items = listed.objects.map((o) => ({
        key: o.key,
        size: o.size,
        uploaded: o.uploaded,
        metadata: o.customMetadata || {},
      }));
      return jsonResponse({ count: items.length, truncated: listed.truncated, items });
    }

    // OTA resolve endpoint: /<version>/<filename>.bin
    const match = path.match(/^\/([^/]+)\/([^/]+\.bin)$/);
    if (!match) {
      return new Response("Not found.", { status: 404 });
    }

    const version = match[1];
    const filename = match[2];

    try {
      for (const tag of [`v${version}`, version]) {
        const downloadUrl = `https://github.com/${GITHUB_OWNER}/${GITHUB_REPO}/releases/download/${tag}/${filename}`;
        const resp = await fetch(downloadUrl, {
          headers: { "User-Agent": "RDM7-OTA-Proxy" },
          redirect: "follow",
        });
        if (!resp.ok) continue;
        return jsonResponse({
          url: resp.url,
          size: parseInt(resp.headers.get("content-length") || "0", 10),
          version: version,
          tag: tag,
        });
      }
      return new Response(`Firmware not found: ${version}/${filename}`, { status: 404 });
    } catch (err) {
      return jsonResponse({ error: err.message }, 500);
    }
  },
};

/* ─── CAN upload handler ─────────────────────────────────────────────────── */

async function handleCanUpload(request, env) {
  if (!env.CAN_LOGS) {
    return jsonResponse({ error: "R2 bucket binding missing on worker" }, 500);
  }
  if (!env.CAN_UPLOAD_HMAC_SECRET) {
    return jsonResponse({ error: "HMAC secret not configured" }, 500);
  }

  const make     = (request.headers.get("X-Make")      || "").trim();
  const model    = (request.headers.get("X-Model")     || "").trim();
  const deviceId = (request.headers.get("X-Device-Id") || "").trim();
  const tsStr    = (request.headers.get("X-Timestamp") || "").trim();
  const sigHex   = (request.headers.get("X-Signature") || "").trim().toLowerCase();
  const notes    = (request.headers.get("X-Notes")     || "").slice(0, 500);

  if (!make || !model || !deviceId || !tsStr || !sigHex) {
    return jsonResponse({ error: "Missing required header(s)" }, 400);
  }

  const ts = parseInt(tsStr, 10);
  if (!Number.isFinite(ts)) {
    return jsonResponse({ error: "Bad X-Timestamp" }, 400);
  }
  if (!/^[0-9a-f]{64}$/.test(sigHex)) {
    return jsonResponse({ error: "Bad X-Signature format" }, 400);
  }

  // Reject stale signatures so the same HMAC can't be replayed forever.
  const now = Math.floor(Date.now() / 1000);
  if (Math.abs(now - ts) > HMAC_TIMESTAMP_WINDOW_S) {
    return jsonResponse({ error: "Timestamp outside acceptable window" }, 401);
  }

  // Canonical HMAC message — same string built on the device.
  const message = `${make}\n${model}\n${deviceId}\n${ts}`;
  const ok = await verifyHmac(env.CAN_UPLOAD_HMAC_SECRET, message, sigHex);
  if (!ok) return jsonResponse({ error: "Invalid signature" }, 401);

  const body = await request.arrayBuffer();
  if (body.byteLength === 0) return jsonResponse({ error: "Empty body" }, 400);
  if (body.byteLength > CAN_UPLOAD_MAX_BYTES) {
    return jsonResponse({ error: `File too large (max ${CAN_UPLOAD_MAX_BYTES} bytes)` }, 413);
  }

  const slug = (s) => s.toLowerCase().replace(/[^a-z0-9_-]+/g, "_").slice(0, 40);
  const key = `${slug(make)}/${slug(model)}/${slug(deviceId)}_${ts}.csv`;

  await env.CAN_LOGS.put(key, body, {
    httpMetadata: { contentType: "text/csv" },
    customMetadata: {
      make, model, device_id: deviceId,
      timestamp: String(ts),
      notes: notes,
      size_bytes: String(body.byteLength),
    },
  });

  return jsonResponse({ ok: true, key, size: body.byteLength });
}

async function verifyHmac(secret, message, sigHex) {
  const enc = new TextEncoder();
  const key = await crypto.subtle.importKey(
    "raw", enc.encode(secret),
    { name: "HMAC", hash: "SHA-256" }, false, ["verify"],
  );
  const sigBytes = new Uint8Array(
    sigHex.match(/.{2}/g).map((b) => parseInt(b, 16))
  );
  try {
    return await crypto.subtle.verify("HMAC", key, sigBytes, enc.encode(message));
  } catch {
    return false;
  }
}

function jsonResponse(obj, status = 200) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: {
      "Content-Type": "application/json",
      "Cache-Control": "no-cache",
    },
  });
}
