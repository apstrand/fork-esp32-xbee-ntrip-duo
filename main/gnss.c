#include "gnss.h"
#include "uart.h"
#include "tasks.h"
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_event.h>

static gnss_status_t gnss_status = {0};

static void parse_gga(const char *line) {
    // $GPGGA,time,lat,N,lon,E,fix,sats,hdop,alt,M,geoid,M,age,ref*cs
    int field = 0;
    const char *p = line;
    while (*p) {
        if (*p == ',') {
            field++;
            if (field == 6) { // fix quality
                gnss_status.fix_quality = atoi(p + 1);
            } else if (field == 7) { // sats
                gnss_status.satellites = atoi(p + 1);
                break;
            }
        }
        p++;
    }
}

static void parse_unicore_version(const char *line) {
    // #VERSIONA,...;1,UM980,R4.10Build9173,0*xxxxxxxx
    const char *p = strstr(line, "UM980,");
    if (p) {
        p += 6;
        const char *end = strchr(p, ',');
        if (!end) end = strchr(p, '*');
        if (end) {
            int len = end - p;
            if (len > sizeof(gnss_status.firmware_version) - 1) len = sizeof(gnss_status.firmware_version) - 1;
            strncpy(gnss_status.firmware_version, p, len);
            gnss_status.firmware_version[len] = '\0';
        }
    }
}

static char line_buf[512];
static int line_pos = 0;

static void gnss_uart_handler(void* handler_args, esp_event_base_t base, int32_t length, void* buffer) {
    uint8_t *data = (uint8_t *)buffer;
    for (int i = 0; i < length; i++) {
        if (data[i] == '\r' || data[i] == '\n') {
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                if (strncmp(line_buf, "$GPGGA", 6) == 0 || strncmp(line_buf, "$GNGGA", 6) == 0) {
                    parse_gga(line_buf);
                } else if (strncmp(line_buf, "#VERSIONA", 9) == 0) {
                    parse_unicore_version(line_buf);
                }
                line_pos = 0;
            }
        } else {
            if (line_pos < sizeof(line_buf) - 1) {
                line_buf[line_pos++] = data[i];
            }
        }
    }
}

static void gnss_task(void *pvParameters) {
    while (1) {
        const char *cmd = "\r\nVERSION\r\n";
        uart_write((char *)cmd, strlen(cmd));
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

void gnss_init() {
    uart_register_read_handler(gnss_uart_handler);
    xTaskCreate(gnss_task, "gnss_task", 2048, NULL, TASK_PRIORITY_WIFI_STATUS, NULL);
}

void gnss_get_status(gnss_status_t *status) {
    memcpy(status, &gnss_status, sizeof(gnss_status_t));
}
