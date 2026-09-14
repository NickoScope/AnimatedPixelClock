#pragma once
#include <cstdint>
typedef void (*esp_timer_cb_t)(void *);
typedef int esp_timer_dispatch_t;
#define ESP_TIMER_TASK 0
#define ESP_OK 0
struct esp_timer_create_args_t { esp_timer_cb_t callback; void *arg; esp_timer_dispatch_t dispatch_method; const char *name; bool skip_unhandled_events; };
typedef void *esp_timer_handle_t;
// Host: no timer, so the firmware's own fallback samples from controlLoop().
inline int esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *) { return -1; }
inline int esp_timer_start_periodic(esp_timer_handle_t, uint64_t) { return -1; }
