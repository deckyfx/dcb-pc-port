/* Linked instead of generated/<id>/ until the recompiler has produced output. */
#include <psx/recomp.h>

static void no_functions(PsxContext* ctx) { (void)ctx; }

const RecompFunctionEntry recomp_function_table[] = {{0u, no_functions}};
const uint32_t recomp_function_count = 0u;
