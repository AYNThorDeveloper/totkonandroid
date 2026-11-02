#pragma once
#include "dm.h"
typedef struct {const char *name;void *fn;const char *why;} hle_force_ent;
#ifdef __APPLE__
#define HLE_SECTION "__DATA,totkgfx"
#else
#define HLE_SECTION "totkgfx" /* ELF: bounds via __start_totkgfx/__stop_totkgfx */
#endif
#define HLE_FORCE(mangled,reason,ret,cname,...) static ret cname(__VA_ARGS__); __attribute__((used,section(HLE_SECTION))) static const hle_force_ent ent_##cname={mangled,(void *)cname,reason}; static ret cname(__VA_ARGS__)
