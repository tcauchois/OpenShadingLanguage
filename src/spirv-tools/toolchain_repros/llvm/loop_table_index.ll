target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
; A runtime-indexed load from a constant table, in a loop whose counter is a
; 32-bit phi (zero-extended to index the array). Crashes in "SPIRV legalize
; bitcast pass" (as do 64-bit counters). Compiles with -disable-lsr, so
; loop strength reduction's rewrite of the address seems to be the trigger.
@table = internal addrspace(10) constant [8 x float] [float 0.0, float 1.0, float 2.0, float 3.0, float 4.0, float 5.0, float 6.0, float 7.0]
@n = internal addrspace(10) global i32 0
@out = internal addrspace(10) global float 0.0
define void @main() #0 {
entry:
  %n = load i32, ptr addrspace(10) @n
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  %acc = phi float [ 0.0, %entry ], [ %acc1, %loop ]
  %idx = zext i32 %i to i64
  %p = getelementptr inbounds [8 x float], ptr addrspace(10) @table, i64 0, i64 %idx
  %v = load float, ptr addrspace(10) %p
  %acc1 = fadd float %acc, %v
  %i1 = add i32 %i, 1
  %c = icmp ult i32 %i1, %n
  br i1 %c, label %loop, label %done
done:
  store float %acc1, ptr addrspace(10) @out
  ret void
}
attributes #0 = { "hlsl.numthreads"="1,1,1" "hlsl.shader"="compute" }
