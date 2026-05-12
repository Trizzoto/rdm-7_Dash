#pragma once

/**
 * Shared secret for the CAN upload endpoint on the Cloudflare worker.
 * Must match the value set via `wrangler secret put CAN_UPLOAD_HMAC_SECRET`
 * in tools/cloudflare-ota-proxy/.
 *
 * DEV-PHASE NOTE: while RDM-7 is pre-customer, this secret lives in the
 * firmware repo. Anyone with a flashed binary can extract it, so don't
 * treat it as protecting anything sensitive — its job is to stop the
 * casual abuser, not a determined one. Rotate before shipping to
 * customers and consider moving to per-device keys derived from
 * device_id.
 */
#define RDM7_CAN_UPLOAD_HMAC_SECRET \
    "9d72ff881ef7ca928b99cece154ee62ae44f521a982a7687bd436cabb64b75b8"

/**
 * Worker URL for the CAN upload endpoint. Same worker as OTA proxy;
 * the path "/can-upload" differentiates.
 */
#define RDM7_CAN_UPLOAD_URL \
    "https://rdm7-ota-proxy.rdm7-ota-proxy.workers.dev/can-upload"
