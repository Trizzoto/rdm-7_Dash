/**
 * Cloudflare Worker: RDM-7 OTA Firmware Proxy
 *
 * Resolves GitHub Releases download URL → final CDN URL,
 * then returns it as JSON. The ESP32 fetches the CDN URL directly.
 * This avoids streaming through the worker (free-tier CPU limits)
 * and avoids long redirect URLs that overflow ESP32 buffers.
 *
 * URL pattern: https://<worker>/<version>/esp32-firmware.bin
 * GitHub source: Trizzoto/potato-jubilee (public repo, releases)
 */

const GITHUB_OWNER = "Trizzoto";
const GITHUB_REPO = "potato-jubilee";

export default {
  async fetch(request) {
    const url = new URL(request.url);
    const path = url.pathname;

    // Health check
    if (path === "/" || path === "/health") {
      return new Response(JSON.stringify({ status: "ok", service: "rdm7-ota-proxy" }), {
        headers: { "Content-Type": "application/json" },
      });
    }

    // Resolve endpoint: /<version>/<filename>.bin
    // Returns JSON with the direct CDN download URL
    const match = path.match(/^\/([^/]+)\/([^/]+\.bin)$/);
    if (!match) {
      return new Response("Not found. Use: /<version>/esp32-firmware.bin", { status: 404 });
    }

    const version = match[1];
    const filename = match[2];

    try {
      for (const tag of [`v${version}`, version]) {
        const downloadUrl = `https://github.com/${GITHUB_OWNER}/${GITHUB_REPO}/releases/download/${tag}/${filename}`;

        // Follow all redirects to resolve the final CDN URL
        const resp = await fetch(downloadUrl, {
          headers: { "User-Agent": "RDM7-OTA-Proxy" },
          redirect: "follow",
        });

        if (!resp.ok) continue;

        // Return the resolved CDN URL + metadata as JSON
        // ESP32 will fetch this URL directly
        return new Response(JSON.stringify({
          url: resp.url,
          size: parseInt(resp.headers.get("content-length") || "0", 10),
          version: version,
          tag: tag,
        }), {
          status: 200,
          headers: {
            "Content-Type": "application/json",
            "Cache-Control": "no-cache",
          },
        });
      }

      return new Response(`Firmware not found: ${version}/${filename}`, { status: 404 });
    } catch (err) {
      return new Response(JSON.stringify({ error: err.message }), {
        status: 500,
        headers: { "Content-Type": "application/json" },
      });
    }
  },
};
