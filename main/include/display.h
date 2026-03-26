#ifndef ESP32_XBEE_DISPLAY_H
#define ESP32_XBEE_DISPLAY_H

// Initialise the 128×32 I2C OLED display and start the status update task.
// Does nothing if CONFIG_OLED_ENABLED is not set.
void display_init(void);

#endif // ESP32_XBEE_DISPLAY_H
