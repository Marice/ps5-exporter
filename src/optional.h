/* Functions looked up when they are first called, not when the payload loads.
 *
 * The SDK binds every function it links at start-up: one that this firmware
 * does not export stops the whole payload (sceKernelIccGetThermalAlert does
 * on 13.60). The getters beyond the SDK's own samples go through here, so a
 * missing one costs only its metric. */
#ifndef OPTIONAL_H
#define OPTIONAL_H

/* The function's address, or NULL when this firmware does not export it. */
void* optional_symbol(const char* name);

/* A typed pointer to a libkernel function: OPTIONAL(int (*)(void*), sceKernelGetSocPowerConsumption). */
#define OPTIONAL(type, name) ((type)optional_symbol(#name))

#endif
