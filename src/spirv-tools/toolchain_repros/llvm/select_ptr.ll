target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
; Selecting between pointers to two elements of an array passed by
; reference, then loading through the result (as Imath's Matrix44::inverse
; does when pivoting). Crashes in "SPIRV legalize bitcast pass".
define spir_func float @f(ptr byref([4 x float]) %m, i1 %c) {
  %a = getelementptr inbounds [4 x float], ptr %m, i32 0, i32 1
  %b = getelementptr inbounds [4 x float], ptr %m, i32 0, i32 2
  %p = select i1 %c, ptr %a, ptr %b
  %v = load float, ptr %p, align 4
  ret float %v
}
