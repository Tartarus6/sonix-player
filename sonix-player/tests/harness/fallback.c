// The harness compiles the module the player actually ships, not a copy of
// its logic. Keeping the include here gives the fallback its own translation
// unit while the test source stays in harness.c.
#include "../../src/system/bluetooth/bluetoothd_fallback.c"
