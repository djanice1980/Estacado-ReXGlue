#pragma once

// REXGLUE_GPU_DIAGNOSTICS (CMake) -> REX_GPU_DIAGNOSTICS: diagnostics and work
// counters exist only in measurement builds. Player builds, and targets that
// include these headers without the definition (tests), get 0.
#ifndef REX_GPU_DIAGNOSTICS
#define REX_GPU_DIAGNOSTICS 0
#endif

namespace rex::graphics {

// The same switch for ordinary code. Diagnostic gates on hot paths start with
// `kGpuDiagnostics &&` or sit under `if (kGpuDiagnostics)`: in player builds
// the condition is constant false and the optimizer removes the diagnostic,
// which still compiles (is type-checked) in both builds.
inline constexpr bool kGpuDiagnostics = REX_GPU_DIAGNOSTICS != 0;

}  // namespace rex::graphics
