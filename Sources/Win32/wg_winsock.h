#ifndef WG_WINSOCK_H
#define WG_WINSOCK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct WGWinsock WGWinsock;

WGWinsock *wg_winsock_create(void);
void       wg_winsock_destroy(WGWinsock *ws);

// Handle a WS2_32/WSOCK32 function call. Returns true if handled.
// args[] are the stdcall arguments (32-bit, read from guest stack).
// *out_ret receives the return value to place in EAX.
bool wg_winsock_handle(WGWinsock *ws, const char *fn,
                       uint32_t *args, uint64_t *out_ret,
                       void *blink);

// Access to last Winsock error (set by failed calls).
uint32_t wg_winsock_get_last_error(WGWinsock *ws);

// Tell winsock where the engine mapped the getaddrinfo result scratch region
// (relocated above the image for large 64-bit PEs).
void     wg_winsock_set_gai_base(uint64_t base);
void     wg_winsock_set_last_error(WGWinsock *ws, uint32_t err);

// Reactor backstop: true if any open socket has data ready to read.
bool wg_winsock_any_readable(WGWinsock *ws);

#endif
