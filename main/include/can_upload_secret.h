#pragma once

/**
 * Shared secret for the CAN upload endpoint on the Cloudflare worker.
 * Must match the value set via `wrangler secret put CAN_UPLOAD_HMAC_SECRET`
 * in tools/cloudflare-ota-proxy/.
 *
 * DEV-PHASE NOTE: while RDM-7 is pre-customer, this secret lives in the
 * firmware repo. Anyone with a flashed binary can extract it, so don't
 * treat it as protecting anything sensitive — its job is to stop the
 * casual abuser, not a determined one. ROTATE BEFORE BETA UNITS SHIP —
 * the step-by-step runbook (generation, fleet-first ordering, worker
 * `wrangler secret put`, verification) is in
 * tools/cloudflare-ota-proxy/README.md § "Rotating the HMAC secret".
 * Longer term, consider per-device keys derived from device_id.
 */
/* Rotated 2026-06-12 (pre-beta, per the runbook). The worker side must be
 * updated to match — until `wrangler secret put CAN_UPLOAD_HMAC_SECRET` is
 * run with THIS value, uploads from this firmware 401 (fail-safe). */
#define RDM7_CAN_UPLOAD_HMAC_SECRET \
    "ca7d22c7ae1ad021595d291f170cd133e2957ad1bbf44e73cc5aca5789e5d0d3"

/**
 * Worker URL for the CAN upload endpoint. Same worker as OTA proxy;
 * the path "/can-upload" differentiates.
 */
#define RDM7_CAN_UPLOAD_URL \
    "https://rdm7-ota-proxy.rdm7-ota-proxy.workers.dev/can-upload"
