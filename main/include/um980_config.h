#pragma once

// Send the full UM980 base station configuration command sequence over UART.
// Blocks until all commands have been sent (with inter-command delays).
void um980_configure_base_station(void);
