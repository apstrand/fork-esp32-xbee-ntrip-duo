#ifndef ESP32_XBEE_GNSS_H
#define ESP32_XBEE_GNSS_H

#include <stdint.h>

typedef struct {
    int satellites;
    int fix_quality;
    char firmware_version[64];
} gnss_status_t;

void gnss_init();
void gnss_get_status(gnss_status_t *status);

#endif //ESP32_XBEE_GNSS_H
