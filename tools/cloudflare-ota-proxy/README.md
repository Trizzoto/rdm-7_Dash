# RDM-7 Cloudflare Worker (OTA Proxy + CAN Log Upload)

Single Worker hosting two endpoints:
- **OTA proxy** — resolves GitHub Release URLs so the ESP32 can fetch firmware
  over HTTP without burning internal RAM on TLS.
- **CAN log upload** — accepts SavvyCAN-format CAN traces from deployed dashes
  and stores them in an R2 bucket, keyed by car make/model.

## Flow

1. ESP32 checks GitHub API (HTTPS, small JSON) → finds new version available
2. ESP32 requests `http://<worker>/<version>/esp32-firmware.bin`
3. Worker fetches release asset from GitHub (HTTPS) → streams back over HTTP

## Setup

### 1. Install Wrangler (Cloudflare CLI)
```bash
npm install -g wrangler
wrangler login
```

### 2. Create the R2 bucket for CAN traces
```bash
cd tools/cloudflare-ota-proxy
wrangler r2 bucket create rdm7-can-logs
```

### 3. Set the HMAC secret for the CAN upload endpoint
```bash
wrangler secret put CAN_UPLOAD_HMAC_SECRET
# Paste the value from main/include/can_upload_secret.h (RDM7_CAN_UPLOAD_HMAC_SECRET)
# when prompted.
```

## Rotating the HMAC secret  ⚠️ DO THIS BEFORE BETA UNITS SHIP

The dev-phase secret is committed to this repo's git history, so treat it as
public — rotation is what makes the old value useless, not deleting it from
the file. The firmware and the worker must hold the **same** value; rotation
is a two-sided change with a fleet-coordination step in between.

1. **Generate** a new 64-hex-char secret (32 random bytes):
   ```powershell
   -join ((1..32) | ForEach-Object { '{0:x2}' -f (Get-Random -Max 256) })
   ```
   (or `openssl rand -hex 32`.)
2. **Firmware first:** replace `RDM7_CAN_UPLOAD_HMAC_SECRET` in
   `main/include/can_upload_secret.h`, build, and get the new firmware onto
   **every** device that should keep uploading (flash the bench units, OTA the
   fleet). Devices still on the old firmware keep working during this phase
   because the worker still holds the old secret.
3. **Then the worker:** once the fleet is updated,
   ```bash
   cd tools/cloudflare-ota-proxy
   wrangler secret put CAN_UPLOAD_HMAC_SECRET    # paste the NEW value
   wrangler deploy
   ```
   From this moment old-firmware devices get `401` on upload (fail-safe:
   the dash shows the upload error; nothing else breaks).
4. **Verify:** on an updated device, data-logger modal → "Share Raw CAN" →
   expect success; and confirm the old secret is dead:
   ```bash
   curl -s -X POST https://<worker>/can-upload \
     -H "X-Make: t" -H "X-Model: t" -H "X-Device-Id: t" \
     -H "X-Timestamp: $(date +%s)" -H "X-Signature: deadbeef" \
     --data-binary "x"   # → must be 401
   ```

If a zero-401 window matters later (bigger fleet, slow OTA uptake), extend
`worker.js` to accept a `CAN_UPLOAD_HMAC_SECRET_OLD` secret as a fallback
during the transition and delete it once the fleet has converged. Not worth
the complexity at beta scale (~10 cars).

Longer term (per `can_upload_secret.h`): derive per-device keys from
`device_id` so one extracted key doesn't expose the shared endpoint.

### 4. Deploy the Worker
```bash
wrangler deploy
```

This gives you a URL like: `https://rdm7-ota-proxy.<your-account>.workers.dev`

### 3. Update firmware default URL
In `main/net/ota_handler.c`, update `OTA_DEFAULT_BASE_URL`:
```c
#define OTA_DEFAULT_BASE_URL "http://rdm7-ota-proxy.<your-account>.workers.dev"
```

### 4. (Optional) Custom domain
To use `ota.rdm7.net` instead of workers.dev:
1. Add your domain to Cloudflare DNS
2. Uncomment the `routes` section in `wrangler.toml`
3. Run `wrangler deploy` again

## Publishing a firmware update

1. Build your firmware: `idf.py build`
2. Create a GitHub Release on `Trizzoto/potato-jubilee`:
   - Tag: `v1.2.0` (or whatever version)
   - Attach `build/esp32-firmware.bin` as a release asset named `esp32-firmware.bin`
3. The ESP32 will detect the new version on next check and download via the worker

## Testing

```bash
# Health check
curl http://rdm7-ota-proxy.<your-account>.workers.dev/

# Test firmware download (after creating a release)
curl -o test.bin http://rdm7-ota-proxy.<your-account>.workers.dev/1.0.0/esp32-firmware.bin
```

## Browsing CAN uploads

The R2 bucket `rdm7-can-logs` holds every trace uploaded from a dashboard,
keyed by `<make>/<model>/<device_id>_<unix_ts>.csv`. Browse:

```bash
wrangler r2 object get rdm7-can-logs/toyota/supra/abc123_1700000000.csv \
  --file local.csv
```

Or via the Cloudflare dashboard → R2 → `rdm7-can-logs`. Custom metadata on
each object carries the original make/model/notes for easy filtering.

## CAN upload protocol

`POST /can-upload` with these required headers and the CSV file as the body:

| Header | Meaning |
|---|---|
| `X-Make` | Car manufacturer (free text, sanitised on the server) |
| `X-Model` | Car model |
| `X-Device-Id` | RDM-7 device ID from `device_id` module |
| `X-Timestamp` | Unix seconds, must be within ±10 min of server time |
| `X-Signature` | HMAC-SHA256 hex of `"{make}\n{model}\n{device_id}\n{timestamp}"` |
| `X-Notes` | Optional free text (max 500 chars) |

Body: SavvyCAN GVRET-CSV bytes from `can_raw_logger`. Max 10 MB per upload.

Returns `200 { ok: true, key, size }` on success; `400/401/413` otherwise.

