#ifndef STATUS_LED_H
#define STATUS_LED_H
/* Board status only; enabled for the normal USB -> DRM path. */
void status_led_start(void);
void status_led_frame(void);
void status_led_waiting(void);
#endif
