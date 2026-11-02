#pragma once
#include <malloc.h>
#define malloc_size(pointer) malloc_usable_size((void *)(pointer))
