/* dns_hijack.c — see header for rationale.
 *
 * Implements a minimal, captive-portal-friendly DNS responder. For every
 * incoming A (IPv4) query, we reply with the ESP's AP IP. For AAAA (IPv6)
 * we return NODATA (code NOERROR, zero answers) so clients don't retry.
 * Everything else is ignored.
 *
 * The responder binds INADDR_ANY:53 so it covers both AP and STA
 * interfaces. In practice only the AP interface matters — on STA the
 * real network's DNS is what clients use.
 */

#include "dns_hijack.h"

#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

static const char *TAG = "dns_hijack";

#define DNS_PORT            53
#define DNS_MAX_LEN         512

/* DNS header layout (RFC 1035) — we only read/set the fields we need */
#define DNS_FLAG_QR         0x8000  /* query (0) vs response (1) */
#define DNS_FLAG_AA         0x0400  /* authoritative answer */
#define DNS_FLAG_RD         0x0100  /* recursion desired (echoed back) */
#define DNS_FLAG_RA         0x0080  /* recursion available */
#define DNS_RCODE_NOERROR   0x0000

#define DNS_QTYPE_A         1
#define DNS_QTYPE_AAAA      28

static TaskHandle_t  s_task   = NULL;
static int           s_sock   = -1;
static volatile bool s_running = false;

static uint32_t _ap_ip_nbo(void) {
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip;
    if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK && ip.ip.addr != 0) {
        return ip.ip.addr; /* already network byte order */
    }
    /* Fallback: ESP-IDF default AP IP */
    return htonl(0xC0A80401); /* 192.168.4.1 */
}

/* Skip past a QNAME (a sequence of length-prefixed labels terminated by 0).
 * Pointers are not expected in queries; if we see a compression tag we abort. */
static int _qname_skip(const uint8_t *buf, int buf_len, int off) {
    while (off < buf_len) {
        uint8_t len = buf[off];
        if (len == 0) return off + 1;
        if ((len & 0xC0) != 0) return -1; /* compressed, refuse */
        off += 1 + len;
    }
    return -1;
}

/* Build a response in-place into out_buf. Returns total response size.
 * On malformed queries, returns -1 (caller drops the packet). */
static int _build_response(const uint8_t *req, int req_len,
                           uint8_t *out, int out_cap) {
    if (req_len < 12 || out_cap < req_len) return -1;

    /* Copy the request (header + question) as the start of the response */
    memcpy(out, req, req_len);

    /* Flip QR bit, set AA + RA, clear rcode — keep ID and RD */
    uint16_t flags = (out[2] << 8) | out[3];
    flags |= DNS_FLAG_QR | DNS_FLAG_AA | DNS_FLAG_RA;
    flags &= ~0x000F; /* rcode NOERROR */
    out[2] = flags >> 8;
    out[3] = flags & 0xFF;

    /* Parse question count; we only answer the first one */
    uint16_t qd = (out[4] << 8) | out[5];
    if (qd == 0) return -1;

    /* Walk the question to find qtype */
    int qoff = _qname_skip(req, req_len, 12);
    if (qoff < 0 || qoff + 4 > req_len) return -1;
    uint16_t qtype = (req[qoff] << 8) | req[qoff + 1];
    int question_end = qoff + 4;

    /* Truncate to exactly one question (drop any trailing garbage/OPT) */
    int resp_len = question_end;

    if (qtype == DNS_QTYPE_A) {
        /* Emit one A answer: pointer to qname (c0 0c), type A, class IN,
         * TTL 60, rdlength 4, IP. 16 bytes total. */
        if (resp_len + 16 > out_cap) return -1;
        uint8_t *a = out + resp_len;
        a[0] = 0xC0; a[1] = 0x0C;              /* name = ptr to offset 12 */
        a[2] = 0x00; a[3] = DNS_QTYPE_A;       /* type A */
        a[4] = 0x00; a[5] = 0x01;              /* class IN */
        a[6] = 0x00; a[7] = 0x00;
        a[8] = 0x00; a[9] = 0x3C;              /* TTL 60s */
        a[10] = 0x00; a[11] = 0x04;            /* rdlength 4 */
        uint32_t ip = _ap_ip_nbo();
        memcpy(a + 12, &ip, 4);
        resp_len += 16;
        out[6] = 0x00; out[7] = 0x01;          /* ANCOUNT = 1 */
    } else {
        /* AAAA and everything else: NOERROR with zero answers so the
         * client doesn't retry or fall back to a different resolver. */
        out[6] = 0x00; out[7] = 0x00;          /* ANCOUNT = 0 */
    }

    /* Zero AUTH/ADDL counts — we never include those */
    out[8] = 0x00; out[9] = 0x00;
    out[10] = 0x00; out[11] = 0x00;

    return resp_len;
}

/* TX/RX buffers live in BSS instead of the task stack so the task can run
 * with a smaller stack (internal RAM is tight after WiFi init). Single
 * task, never re-entered, so no locking needed. 1KB of BSS is a fair
 * trade for a task that would otherwise need 3KB+ of stack. */
static uint8_t s_dns_rx[DNS_MAX_LEN];
static uint8_t s_dns_tx[DNS_MAX_LEN];

static void _dns_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "DNS hijack task started on UDP :%d", DNS_PORT);

    while (s_running) {
        struct sockaddr_in src = {0};
        socklen_t src_len = sizeof(src);
        int n = recvfrom(s_sock, s_dns_rx, sizeof(s_dns_rx), 0,
                         (struct sockaddr *)&src, &src_len);
        if (n <= 0) {
            if (!s_running) break;
            /* recvfrom gets interrupted during shutdown — ignore */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int resp = _build_response(s_dns_rx, n, s_dns_tx, sizeof(s_dns_tx));
        if (resp <= 0) continue;
        sendto(s_sock, s_dns_tx, resp, 0, (struct sockaddr *)&src, src_len);
    }

    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    ESP_LOGI(TAG, "DNS hijack task exited");
    TaskHandle_t self = s_task;
    s_task = NULL;
    /* Matches xTaskCreateWithCaps so the PSRAM stack is freed too */
    vTaskDeleteWithCaps(self);
}

esp_err_t dns_hijack_start(void) {
    if (s_running) return ESP_OK;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return ESP_FAIL;
    }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(DNS_PORT),
        .sin_addr   = { .s_addr = htonl(INADDR_ANY) },
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(:53) failed: errno=%d", errno);
        close(sock);
        return ESP_FAIL;
    }

    s_sock    = sock;
    s_running = true;
    /* Stack in PSRAM — internal RAM is ~65KB free and fragmented after
     * WiFi init, so `xTaskCreate` fails. `xTaskCreateWithCaps` lets us
     * place the stack in SPIRAM (8MB available). Task code still runs
     * from flash/IRAM as normal; only the stack storage moves. */
    BaseType_t ok = xTaskCreateWithCaps(_dns_task, "dns_hijack",
                                         3072, NULL, 3, &s_task,
                                         MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed (even in PSRAM, ok=%d)", (int)ok);
        s_running = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Task created with 3072-byte PSRAM stack");
    ESP_LOGI(TAG, "DNS hijack started");
    return ESP_OK;
}

void dns_hijack_stop(void) {
    if (!s_running) return;
    s_running = false;
    if (s_sock >= 0) {
        /* Close while task is in recvfrom — unblocks it */
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
        s_sock = -1;
    }
    /* Task self-deletes when it exits the recv loop */
}
