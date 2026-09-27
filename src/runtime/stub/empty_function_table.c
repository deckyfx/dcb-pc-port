/* Linked instead of generated/<id>/ until the recompiler has produced output. */
#include <psx/coverage.h>
#include <psx/recomp.h>

static void no_functions(PsxContext* ctx) { (void)ctx; }

const RecompFunctionEntry recomp_function_table[] = {{0u, no_functions}};
const uint32_t recomp_function_count = 0u;

const RecompFunctionEntry recomp_original_table[] = {{0u, no_functions}};
const uint32_t recomp_original_count = 0u;

const RecompOverlay recomp_overlays[] = {{"", 0u, 0u, 0, 0u}};
const uint32_t recomp_overlay_count = 0u;

const PsxCoverageName psx_coverage_names[] = {{0u, "", ""}};
const uint32_t psx_coverage_name_count = 0u;
