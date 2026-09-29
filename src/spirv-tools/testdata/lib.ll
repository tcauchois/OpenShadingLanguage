define spir_func void @eval_osl(float %u) {
  %s = call float @llvm.sin.f32(float %u)
  ret void
}
declare float @llvm.sin.f32(float)
