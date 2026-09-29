target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
; A byte-offset (i8) GEP to a field of a struct passed by reference, as the
; optimizer canonicalizes struct accesses. Crashes in "SPIRV legalize
; bitcast pass"; the typed equivalent (getelementptr %S, ptr %p, i32 0,
; i32 1) compiles. (A library function, as OSL's shader groups are.)
%S = type { float, float, i32 }
define spir_func float @f(ptr byref(%S) %p) {
  %q = getelementptr inbounds i8, ptr %p, i64 4
  %v = load float, ptr %q, align 4
  ret float %v
}
