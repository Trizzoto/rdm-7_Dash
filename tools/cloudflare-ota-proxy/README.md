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

