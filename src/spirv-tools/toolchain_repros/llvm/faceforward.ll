target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
; Two float selects on constant conditions crash the pre-legalizer
; combiner's select-to-FaceForward rule (rule 1), with a null dereference.
; Compiles with -spirvprelegalizercombiner-disable-rule=1. (Reduced from
; OSL's gabor noise.)
@out = internal addrspace(10) global float 0.0
define void @main() #0 {
  %c = fcmp ogt float 0.0, 0.0
  %a = select i1 %c, float 0.0, float 0.0
  %b = select i1 false, float 0.0, float 0.0
  store float %b, ptr addrspace(10) @out
  ret void
}
attributes #0 = { "hlsl.numthreads"="1,1,1" "hlsl.shader"="compute" }
