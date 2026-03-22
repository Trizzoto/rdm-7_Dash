# RDM-7 OTA Proxy (Cloudflare Worker)

Proxies firmware downloads from GitHub Releases over plain HTTP for the ESP32
(which can't do HTTPS for large files due to internal RAM limits).

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

### 2. Deploy the Worker
```bash
cd tools/cloudflare-ota-proxy
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
