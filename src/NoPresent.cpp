#include <cstdint>
// Upscaling only: this build never hooks the swap chain or generates frames.
void PresentEvent(int) {}
bool PresentHooked() { return false; }
uint64_t PresentsSeen() { return 0; }
