#include "um980_config.h"
#include "uart.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

static const char *TAG = "UM980";

// Recommended command sequence based on Unicore documentation and Onocoy community
// guidelines:
//   UNLOGALL         clear any previous scheduled outputs
//   MODE BASE TIME   survey for up to 600 s or until 1.5 m accuracy (tune as needed)
//   CONFIG ANTHEIGHT set antenna height above ground (metres)
//   RTCM1006 5       Reference station ARP with antenna height every 5 s
//   RTCM1077 1       GPS MSM7 every 1 s
//   RTCM1087 1       GLONASS MSM7 every 1 s
//   RTCM1097 1       Galileo MSM7 every 1 s
//   RTCM1127 1       BeiDou MSM7 every 1 s
//   RTCM1117 1       QZSS MSM7 every 1 s (no output outside Asia-Pacific; harmless elsewhere)
//   RTCM1230 10      GLONASS code-phase biases every 10 s (improves mixed-brand rovers)
//   SAVECONFIG       persist to flash so settings survive power cycles
//
// Each command is preceded by \r\n and followed by \r\n. The delays between
// commands give the receiver time to acknowledge before the next command arrives.
// The sequence is triggered by a web UI button, NOT at every boot, so the receiver
// is only re-configured on demand and SAVECONFIG persists across resets.

static const struct {
    const char *cmd;
    int delay_ms;
} um980_config_cmds[] = {
    {"\r\nUNLOGALL\r\n",               200},
    {"\r\nMODE BASE TIME 600 1.5\r\n", 200},
    {"\r\nCONFIG ANTHEIGHT 0.0\r\n",   100},
    {"\r\nRTCM1005 1\r\n",             100},
    {"\r\nRTCM1006 5\r\n",             100},
    {"\r\nRTCM1074 1\r\n",             100},
    {"\r\nRTCM1077 1\r\n",             100},
    {"\r\nRTCM1086 1\r\n",             100},
    {"\r\nRTCM1087 1\r\n",             100},
    {"\r\nRTCM1097 1\r\n",             100},
    {"\r\nRTCM1117 1\r\n",             100},
    {"\r\nRTCM1127 1\r\n",             100},
    {"\r\nRTCM1137 1\r\n",             100},
    {"\r\nRTCM1230 10\r\n",            100},
    {"\r\nVERSION\r\n",               200},
    {"\r\nSAVECONFIG\r\n",             200},
};

void um980_configure_base_station(void) {
    ESP_LOGI(TAG, "Sending base station configuration");
    for (int i = 0; i < sizeof(um980_config_cmds) / sizeof(um980_config_cmds[0]); i++) {
        const char *cmd = um980_config_cmds[i].cmd;
        uart_write((char *)cmd, strlen(cmd));
        vTaskDelay(pdMS_TO_TICKS(um980_config_cmds[i].delay_ms));
    }
    ESP_LOGI(TAG, "Base station configuration sent");
}
