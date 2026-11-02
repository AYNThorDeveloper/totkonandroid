#pragma once
#include <stddef.h>
#include <stdint.h>
/* Returns a malloc-owned reply, or NULL with an error logged. Calls are serialized by the renderer shader lock. */
uint8_t *nvk_translate_local(const char *request, const char *cache, size_t *length);
