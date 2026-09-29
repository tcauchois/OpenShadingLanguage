target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
; A loop over a constant table of 3-float structs, with a 64-bit counter,
; reading one field, all with typed GEPs. Crashes in "SPIRV legalize
; bitcast pass"; compiles with -disable-lsr.
%Vec3 = type { float, float, float }
@table = internal addrspace(10) constant [4 x %Vec3] [%Vec3 { float 1.0, float 0.0, float 0.0 }, %Vec3 { float 0.0, float 1.0, float 0.0 }, %Vec3 { float 0.0, float 0.0, float 1.0 }, %Vec3 { float 1.0, float 1.0, float 1.0 }]
@n = internal addrspace(10) global i64 0
@out = internal addrspace(10) global float 0.0
define void @main() #0 {
entry:
  %n = load i64, ptr addrspace(10) @n
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i1, %loop ]
  %acc = phi float [ 0.0, %entry ], [ %acc1, %loop ]
  %p = getelementptr inbounds [4 x %Vec3], ptr addrspace(10) @table, i64 0, i64 %i, i32 1
  %v = load float, ptr addrspace(10) %p
  %acc1 = fadd float %acc, %v
  %i1 = add i64 %i, 1
  %c = icmp ult i64 %i1, %n
  br i1 %c, label %loop, label %done
done:
  store float %acc1, ptr addrspace(10) @out
  ret void
}
attributes #0 = { "hlsl.numthreads"="1,1,1" "hlsl.shader"="compute" }
