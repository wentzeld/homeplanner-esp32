// Minimal captive-portal DNS: answers every A query with the hotspot's address so phones
// open the setup page automatically.
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns";
static const uint8_t AP_IP[4] = {192, 168, 4, 1};

static void dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "can't bind port 53");
        vTaskDelete(NULL);
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof from;
        int len = recvfrom(sock, buf, sizeof buf - 16, 0, (struct sockaddr *)&from, &from_len);
        if (len < 12) continue;
        // Find the end of the first question (QNAME labels + QTYPE + QCLASS).
        int pos = 12;
        while (pos < len && buf[pos] != 0) pos += buf[pos] + 1;
        pos += 5;
        if (pos > len) continue;
        uint16_t qtype = (buf[pos - 4] << 8) | buf[pos - 3];
        buf[2] = 0x81;  // response, recursion desired
        buf[3] = 0x80;  // recursion available, no error
        buf[6] = 0, buf[7] = qtype == 1 ? 1 : 0;  // one answer for A queries
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        len = pos;  // drop any additional records
        if (qtype == 1) {
            const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4,
                                      AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]};
            memcpy(buf + len, answer, sizeof answer);
            len += sizeof answer;
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, from_len);
    }
}

void dns_start(void) { xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL); }
