target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"
; Passing a pointer to a field of a struct to a function taking a byref
; argument: the OpFunctionCall gets an access chain as its pointer
; operand, which spirv-val rejects ("Pointer operand must be a memory object
; declaration").
%Inner = type { i32, [4 x i32] }
%Outer = type { [4 x i8], %Inner }
@o = internal addrspace(10) global %Outer zeroinitializer
@out = internal addrspace(10) global i32 0
define internal spir_func i32 @callee(ptr byref(%Inner) %p) noinline {
  %v = load i32, ptr %p, align 4
  ret i32 %v
}
define void @main() #0 {
  %o = alloca %Outer
  %f = getelementptr inbounds %Outer, ptr %o, i32 0, i32 1
  %v = call spir_func i32 @callee(ptr byref(%Inner) %f)
  store i32 %v, ptr addrspace(10) @out
  ret void
}
attributes #0 = { "hlsl.numthreads"="1,1,1" "hlsl.shader"="compute" }
