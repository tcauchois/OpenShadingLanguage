target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
; The same constant select in a branch and after it: the backend combines
; them into one OpSelect, which it emits in the header block between the
; OpSelectionMerge and the OpBranchConditional (where nothing is allowed).
@in = internal addrspace(10) global i32 0
@out = internal addrspace(10) global i32 0
@out2 = internal addrspace(10) global float 0.0
define void @main() #0 {
entry:
  %x = load i32, ptr addrspace(10) @in
  %c = icmp sgt i32 %x, 0
  br i1 %c, label %then, label %join
then:
  %s = select i1 false, float 1.0, float 0.0
  %b = bitcast float %s to i32
  store i32 %b, ptr addrspace(10) @out
  br label %join
join:
  %t = select i1 false, float 1.0, float 0.0
  store float %t, ptr addrspace(10) @out2
  ret void
}
attributes #0 = { "hlsl.numthreads"="1,1,1" "hlsl.shader"="compute" }
