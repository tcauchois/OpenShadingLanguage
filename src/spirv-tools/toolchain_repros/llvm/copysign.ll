target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
; llvm.copysign can't be selected ("cannot select: G_FCOPYSIGN").
declare float @llvm.copysign.f32(float, float)
@in = internal addrspace(10) global [2 x float] zeroinitializer
@out = internal addrspace(10) global float 0.0
define void @main() #0 {
  %pa = getelementptr inbounds [2 x float], ptr addrspace(10) @in, i32 0, i32 0
  %pb = getelementptr inbounds [2 x float], ptr addrspace(10) @in, i32 0, i32 1
  %a = load float, ptr addrspace(10) %pa
  %b = load float, ptr addrspace(10) %pb
  %r = call float @llvm.copysign.f32(float %a, float %b)
  store float %r, ptr addrspace(10) @out
  ret void
}
attributes #0 = { "hlsl.numthreads"="1,1,1" "hlsl.shader"="compute" }
