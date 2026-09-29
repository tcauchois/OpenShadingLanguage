target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
; Comparing a (non-null) pointer with null.
%S = type { float }
@out = internal addrspace(10) global i32 0
define void @main() #0 {
  %s = alloca %S
  %c = icmp ne ptr %s, null
  %r = zext i1 %c to i32
  store i32 %r, ptr addrspace(10) @out
  ret void
}
attributes #0 = { "hlsl.numthreads"="1,1,1" "hlsl.shader"="compute" }
