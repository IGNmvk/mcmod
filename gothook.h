/* Minimal GOT/PLT hook: redirects one imported function inside one loaded library. */
#ifndef NC_GOTHOOK_H
#define NC_GOTHOOK_H
#ifdef __cplusplus
extern "C" {
#endif
/* lib_part: part of the library path, e.g. "libminecraftpe.so"
 * Returns the number of GOT slots patched, -1 if the library is not loaded, 0 if the symbol is not imported.
 * *original receives the original function pointer (set BEFORE any slot is patched). */
int nc_got_hook(const char *lib_part, const char *symbol, void *replacement, void **original);
#ifdef __cplusplus
}
#endif
#endif
