#include "../../../common/module_devices.h"
void board_configure_devices(void) {
    /* No Goldfish RTC is implied by this architecture. Honor one only when
     * the actual firmware describes it; the old 0x101000 preset was fictitious. */
    board_firmware_user_owned(0, "google,goldfish-rtc", 1);
}
