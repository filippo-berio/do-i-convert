#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t size;
    size_t len;
    size_t cap;
    void *ptr;
} DA;

size_t da_append(DA *da, void *item) {
	if (da->cap <= da->len) {
	    da->cap = da->cap*2;
		da->ptr = realloc(da->ptr, da->cap*da->size);
	}
	memcpy(da->ptr + da->len*da->size, item, da->size);
	return ++da->len;
}

void da_reset(DA *da) {
    memset(da->ptr, 0, da->size * da->cap);
    da->len = 0;
}

void *da_alloc(DA *da, size_t cap, size_t size) {
    da->ptr = malloc(size*cap);
    da->cap = cap;
    da->size = size;
    da_reset(da);
    return da->ptr;
}


void da_free(DA *da) {
    free(da->ptr);
    da->cap = 0;
    da->len = 0;
}

void *da_at(DA da, size_t i) {
    return &da.ptr[i*da.size];
}
