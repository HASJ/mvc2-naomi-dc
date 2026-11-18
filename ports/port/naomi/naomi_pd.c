/*
 * naomi_pd.c  The arcade program's Shinobi controller calls. Its controls come through the JVS; only with the
 * test menu's VMS mode on (an EEPROM can hold it) does it ask Shinobi for pads, memory cards and vibration
 * packs, and the port has none of them: no socket holds a device.
 */

#include "shinobi.h"

static PDS_PERIPHERALINFO none_info;
static PDS_PERIPHERAL none = { 0, 0, 0, 0xFFFFFFFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, &none_info };

PDS_PERIPHERAL* pdGetPeripheral(Uint32 port) {
    (void)port;
    return &none;
}

PDS_PERIPHERALINFO* pdGetPeripheralInfo(Uint32 port) {
    (void)port;
    return &none_info;
}
