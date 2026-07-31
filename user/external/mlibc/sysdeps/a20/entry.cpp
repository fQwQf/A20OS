/*
 * A20 mlibc entry: crt0 hands us the a20_start_info_t pointer (native ABI
 * startup protocol, docs/native-abi/07-startup.md).  We synthesize the
 * classic exec stack layout that mlibc's static rtld expects and enter it.
 */
#include "a20.hpp"

#include <stdint.h>
#include <stdlib.h>
#include <mlibc/elf/startup.h>

extern "C" void __dlapi_enter(uintptr_t *);

extern char **environ;

extern "C" const a20_start_info *__a20_start_info = nullptr;

/* Normally provided by crtbegin.o; we link without it. */
extern "C" void *__dso_handle __attribute__((visibility("hidden"))) = &__dso_handle;

extern "C" void __a20_mlibc_entry(const a20_start_info *si,
                                  int (*main_fn)(int argc, char *argv[], char *env[])) {
	__a20_start_info = si;

	/* Synthesize [argc][argv...][NULL][envp...][NULL][auxv...][AT_NULL].
	 * auxv carries AT_PAGESZ/AT_SECURE; AT_PHDR is unnecessary in static
	 * builds (mlibc locates phdrs via __ehdr_start). */
	static uintptr_t fake_stack[256];
	char **argv = (char **)si->argv;
	char **envp = (char **)si->envp;
	uint32_t argc = si->argc;
	uint32_t envc = si->envc;
	if (!argv) argc = 0;
	if (!envp) envc = 0;
	if ((uint64_t)argc + (uint64_t)envc + 8 > 256) {
		argc = 0;
		envc = 0;
	}

	size_t i = 0;
	fake_stack[i++] = argc;
	for (uint32_t n = 0; n < argc; n++)
		fake_stack[i++] = (uintptr_t)argv[n];
	fake_stack[i++] = 0;
	for (uint32_t n = 0; n < envc; n++)
		fake_stack[i++] = (uintptr_t)envp[n];
	fake_stack[i++] = 0;
	fake_stack[i++] = 6; /* AT_PAGESZ */
	fake_stack[i++] = si->page_size ? si->page_size : 4096;
	fake_stack[i++] = 23; /* AT_SECURE */
	fake_stack[i++] = 0;
	fake_stack[i++] = 0; /* AT_NULL */
	fake_stack[i++] = 0;

	__dlapi_enter(fake_stack);

	auto result = main_fn(mlibc::entry_stack.argc, mlibc::entry_stack.argv, environ);
	exit(result);
}
