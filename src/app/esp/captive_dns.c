#include "captive_dns.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns";
static TaskHandle_t s_task;
static volatile bool s_run;

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (s_run) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        /* only standard queries with one question */
        if ((buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) continue;
        /* find end of QNAME */
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5;   /* zero byte + QTYPE + QCLASS */
        if (p > n) continue;
        uint16_t qtype = (buf[p - 4] << 8) | buf[p - 3];

        buf[2] = 0x84 | (buf[2] & 0x01);   /* response, authoritative, keep RD */
        buf[3] = 0x80;                     /* RA, no error */
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0;   /* ANCOUNT */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = p;
        if (qtype == 1) {
            const uint8_t ans[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
                                   0x00, 0x04, 192, 168, 4, 1};
            memcpy(buf + p, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, flen);
    }
    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

void captive_dns_start(void)
{
    if (s_task) return;
    s_run = true;
    xTaskCreate(dns_task, "cap_dns", 3072, NULL, 4, &s_task);
}

void captive_dns_stop(void)
{
    s_run = false;
}
