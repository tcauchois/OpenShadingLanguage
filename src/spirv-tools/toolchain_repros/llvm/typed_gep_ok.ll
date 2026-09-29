target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
; The control for untyped_gep.ll: the same access with a typed GEP compiles.
%S = type { float, float, i32 }
define spir_func float @f(ptr byref(%S) %p) {
  %q = getelementptr inbounds %S, ptr %p, i32 0, i32 1
  %v = load float, ptr %q, align 4
  ret float %v
}
