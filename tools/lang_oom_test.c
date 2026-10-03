#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pico.h"
#include "port.h"

static int allocation, fail_at;
static bool poison;

void *__real_port_alloc(size_t n);

void *__wrap_port_alloc(size_t n)
{
	void *p;

	if (fail_at && ++allocation == fail_at)
		return NULL;
	p = __real_port_alloc(n);
	if (p && poison)
		memset(p, 0xa5, n);
	return p;
}

int main(void)
{
	const char source[] = "int main() { println(42); return 0; }";
	struct pico_image image = { 0 };
	char err[200];

	assert(!pico_compile("oom.pico", source, strlen(source), &image));
	for (int failure = 1; failure <= 5; failure++) {
		struct pico_vm *vm = pico_vm_new();

		assert(vm);
		allocation = 0;
		fail_at = failure;
		poison = true;
		assert(pico_load(vm, image.data, image.len, err, sizeof(err)));
		assert(strstr(err, "out of memory"));
		fail_at = 0;
		poison = false;
		pico_vm_free(vm, false);
	}
	port_free(image.data);
	puts("language: failed image-table allocations clean up poisoned memory safely");
	return 0;
}
