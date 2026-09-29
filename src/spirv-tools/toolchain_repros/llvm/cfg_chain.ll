target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
; An if/else-if chain whose branches jump to a common exit, with an early
; exit from the middle, as the optimizer produces from OSL's code. The
; backend's own structurizer emits invalid structured control flow.
@in = internal addrspace(10) global i32 0
@out = internal addrspace(10) global i32 0
define void @main() #0 {
entry:
  %x = load i32, ptr addrspace(10) @in
  %c0 = icmp eq i32 %x, 0
  br i1 %c0, label %a, label %t1
t1:
  %c1 = icmp eq i32 %x, 1
  br i1 %c1, label %b, label %t2
t2:
  %c2 = icmp eq i32 %x, 2
  br i1 %c2, label %exit, label %c
a:
  store i32 10, ptr addrspace(10) @out
  %ca = icmp sgt i32 %x, 5
  br i1 %ca, label %exit, label %b
b:
  store i32 20, ptr addrspace(10) @out
  br label %exit
c:
  store i32 30, ptr addrspace(10) @out
  br label %exit
exit:
  ret void
}
attributes #0 = { "hlsl.numthreads"="1,1,1" "hlsl.shader"="compute" }
