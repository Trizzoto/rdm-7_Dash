/*
 * dns_hijack.c — Minimal captive-portal DNS responder. See dns_hijack.h.
 *
 * For every UDP :53 query we echo the request back as an authoritative response
 * with one A record pointing at 192.168.4.1, regardless of the queried name.
 * That's the standard "DNS hijack" captive pattern: the client's OS resolves its
 * connectivity-probe hostname to us, its probe HTTP request hits our captive
 * handler, and the captive sheet opens. Stateless, one small task.
 */
#include "dns_hijack.h"

#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "dns_hijack";

static TaskHandle_t   s_task = NULL;
static volatile bool  s_run  = false;

/* Answer appended after the (echoed) question: name pointer to offset 12,
 * TYPE A, CLASS IN, TTL 60 s, RDLENGTH 4, RDATA 192.168.4.1. */
static const uint8_t ANSWER[] = {
    0xC0, 0x0C,             /* name: pointer to the question's QNAME at offset 12 */
    0x00, 0x01,             /* TYPE  = A */
    0x00, 0x01,             /* CLASS = IN */
    0x00, 0x00, 0x00, 0x3C, /* TTL   = 60 s */
    0x00, 0x04,             /* RDLENGTH = 4 */
    192, 168, 4, 1          /* RDATA = 192.168.4.1 */
};

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed; captive DNS disabled");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = { 0 };
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(53);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "bind :53 failed; captive DNS disabled");
        close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* 1 s recv timeout so we notice dns_hijack_stop() promptly. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "captive DNS up on :53 -> 192.168.4.1");

    uint8_t buf[512];
    while (s_run) {
        struct sockaddr_in from;
        socklen_t          flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 12 || n > (int)(sizeof(buf) - sizeof(ANSWER))) continue;  /* incl. recv timeout (<0) */

        /* Walk the first question's QNAME (length-prefixed labels, 0 terminator). */
        int p = 12;
        while (p < n && buf[p] != 0) {
            p += buf[p] + 1;
            if (p >= n) break;
        }
        if (p >= n) continue;            /* malformed */
        p += 1 + 4;                      /* null label + QTYPE + QCLASS */
        if (p + (int)sizeof(ANSWER) > (int)sizeof(buf) || p > n) continue;

        /* Turn the query into an authoritative response with one answer. */
        buf[2] = 0x84 | (buf[2] & 0x01); /* QR=1, AA=1, preserve RD */
        buf[3] = 0x80;                   /* RA=1, RCODE=0 */
        buf[6] = 0x00; buf[7] = 0x01;    /* ANCOUNT = 1 */
        buf[8] = 0x00; buf[9] = 0x00;    /* NSCOUNT = 0 */
        buf[10] = 0x00; buf[11] = 0x00;  /* ARCOUNT = 0 */
        memcpy(buf + p, ANSWER, sizeof(ANSWER));

        sendto(sock, buf, p + sizeof(ANSWER), 0, (struct sockaddr *)&from, flen);
    }

    close(sock);
    ESP_LOGI(TAG, "captive DNS stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

void dns_hijack_start(void)
{
    if (s_task) return;
    s_run = true;
    if (xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 5, &s_task) != pdPASS) {
        s_run  = false;
        s_task = NULL;
        ESP_LOGW(TAG, "failed to create captive DNS task");
    }
}

void dns_hijack_stop(void)
{
    s_run = false;   /* task observes this within its 1 s recv timeout and self-deletes */
}
