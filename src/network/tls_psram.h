#pragma once
// mbedTLS allocations on PSRAM instead of internal SRAM. See tls_psram.cpp.

#if defined(BOARD_HAS_PSRAM)
void tlsUsePsram();   // call before anything opens a TLS session
#endif
