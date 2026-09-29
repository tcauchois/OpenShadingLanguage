; ModuleID = 'unnamed_group_9_spirv.ll'
source_filename = "llvm-link"
target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024-n8:16:32:64-G10"
target triple = "spirv1.6-unknown-vulkan1.3-compute"

%"struct.(anonymous namespace)::SpirvMessageArgs" = type { [64 x i32], [128 x i32] }
%ShaderGlobals = type { [3 x %Vec3], %Vec3, [3 x %Vec3], %Vec3, %Vec3, [3 x float], [3 x float], %Vec3, %Vec3, float, float, %Vec3, [3 x %Vec3], i64, i64, i64, i64, i64, i32, i32, i64, i64, i64, i64, float, i32, i32, i32 }
%Vec3 = type { float, float, float }
%Groupdata = type { [4 x i1], %OSLClosurePool }
%OSLClosurePool = type { i32, [32 x i32] }

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare spir_func float @llvm.floor.f32(float) #0

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare spir_func void @llvm.lifetime.start.p0(ptr captures(none)) #1

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(argmem: readwrite)
declare spir_func void @llvm.lifetime.end.p0(ptr captures(none)) #1

declare spir_func void @osl_spirv_message(i32 noundef, i32 noundef, i64 noundef, i64 noundef, i32 noundef, ptr byref(%"struct.(anonymous namespace)::SpirvMessageArgs")) local_unnamed_addr #2

; Function Attrs: nounwind
define spir_func void @eval_osl(ptr byref(%ShaderGlobals) %0, ptr byref(%Groupdata) %1) local_unnamed_addr #3 {
bb_eval_osl_2:
  %2 = alloca %"struct.(anonymous namespace)::SpirvMessageArgs", align 4
  %3 = alloca %"struct.(anonymous namespace)::SpirvMessageArgs", align 4
  %4 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 0
  store i32 0, ptr %4, align 4
  %5 = getelementptr inbounds %ShaderGlobals, ptr %0, i32 0, i32 5, i32 0
  %u_.i = load float, ptr %5, align 4
  %6 = fmul float %u_.i, 8.000000e+00
  %7 = fmul float %6, 2.000000e+00
  %8 = tail call reassoc nsz arcp contract afn spir_func noundef float @llvm.floor.f32(float %7)
  %9 = fptosi float %8 to i32
  %10 = getelementptr inbounds %ShaderGlobals, ptr %0, i32 0, i32 6, i32 0
  %v_.i = load float, ptr %10, align 4
  %11 = fmul float %v_.i, 8.000000e+00
  %12 = tail call reassoc nsz arcp contract afn spir_func noundef float @llvm.floor.f32(float %11)
  %13 = fptosi float %12 to i32
  %14 = add i32 %9, %13
  %15 = srem i32 %14, 2
  %16 = sitofp i32 %15 to float
  %17 = fsub float 1.000000e+00, %16
  %18 = fmul float %17, 0x3FE99999A0000000
  %19 = fmul float %16, 0x3FD47AE160000000
  %20 = fadd float %19, %18
  %21 = fmul float %17, 0x3FD3333340000000
  %22 = fmul float %16, 0x3FBEB85200000000
  %23 = fadd float %22, %21
  %24 = fmul float %17, 0x3FC99999A0000000
  %25 = fmul float %16, 0x3FB47AE160000000
  %26 = fadd float %25, %24
  %27 = fcmp reassoc nsz arcp contract afn oeq float %20, 0.000000e+00
  %28 = fcmp reassoc nsz arcp contract afn oeq float %23, 0.000000e+00
  %29 = and i1 %27, %28
  %30 = fcmp reassoc nsz arcp contract afn oeq float %26, 0.000000e+00
  %31 = and i1 %30, %29
  %.inv7 = xor i1 %31, true
  br i1 %.inv7, label %32, label %osl_spirv_allocate_closure_component.exit8.i

32:                                               ; preds = %bb_eval_osl_2
  %33 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 0
  store i32 10, ptr %33, align 4, !tbaa !1
  %34 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 0
  store i32 2, ptr %34, align 4, !tbaa !1
  %35 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 1
  %36 = bitcast float %20 to i32
  store i32 %36, ptr %35, align 4
  %37 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 2
  %38 = bitcast float %23 to i32
  store i32 %38, ptr %37, align 4
  %39 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 3
  %40 = bitcast float %26 to i32
  store i32 %40, ptr %39, align 4
  %41 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 4
  store i32 0, ptr %41, align 4
  %42 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 5
  store i32 0, ptr %42, align 4
  %43 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 6
  store i32 0, ptr %43, align 4
  %44 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 7
  store i32 0, ptr %44, align 4
  %45 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 8
  store i32 0, ptr %45, align 4
  %46 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 9
  store i32 0, ptr %46, align 4
  br label %osl_spirv_allocate_closure_component.exit8.i

osl_spirv_allocate_closure_component.exit8.i:     ; preds = %32, %bb_eval_osl_2
  %47 = phi i64 [ 0, %bb_eval_osl_2 ], [ 1, %32 ]
  br i1 %.inv7, label %48, label %osl_spirv_closure_param.exit15.i

48:                                               ; preds = %osl_spirv_allocate_closure_component.exit8.i
  %49 = getelementptr inbounds %ShaderGlobals, ptr %0, i32 0, i32 3, i32 0
  %50 = load float, ptr %49, align 4
  %51 = bitcast float %50 to i32
  %52 = trunc i64 %47 to i32
  %53 = add i32 %52, 3
  %54 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %53
  store i32 %51, ptr %54, align 4
  %55 = getelementptr inbounds %ShaderGlobals, ptr %0, i32 0, i32 3, i32 1
  %56 = load float, ptr %55, align 4
  %57 = bitcast float %56 to i32
  %58 = trunc i64 %47 to i32
  %59 = add i32 %58, 4
  %60 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %59
  store i32 %57, ptr %60, align 4
  %61 = getelementptr inbounds %ShaderGlobals, ptr %0, i32 0, i32 3, i32 2
  %62 = load float, ptr %61, align 4
  %63 = bitcast float %62 to i32
  %64 = trunc i64 %47 to i32
  %65 = add i32 %64, 5
  %66 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %65
  store i32 %63, ptr %66, align 4
  br label %osl_spirv_closure_param.exit15.i

osl_spirv_closure_param.exit15.i:                 ; preds = %48, %osl_spirv_allocate_closure_component.exit8.i
  %67 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 0
  %68 = load i32, ptr %67, align 4, !tbaa !1
  %69 = icmp sle i32 %68, 16
  br i1 %69, label %75, label %Flow6

Flow6:                                            ; preds = %Flow4, %osl_spirv_closure_param.exit15.i
  %70 = phi i64 [ %143, %Flow4 ], [ poison, %osl_spirv_closure_param.exit15.i ]
  %71 = phi i1 [ false, %Flow4 ], [ true, %osl_spirv_closure_param.exit15.i ]
  br i1 %71, label %72, label %osl_spirv_allocate_closure_component.exit.i

72:                                               ; preds = %Flow6
  call spir_func void @llvm.lifetime.start.p0(ptr nonnull %3) #5
  %73 = getelementptr inbounds %ShaderGlobals, ptr %0, i32 0, i32 19
  %74 = load i32, ptr %73, align 4, !tbaa !9
  call spir_func void @osl_spirv_message(i32 noundef 0, i32 noundef %74, i64 noundef -2300610049087590229, i64 noundef 0, i32 noundef 0, ptr noundef nonnull %3) #5
  call spir_func void @llvm.lifetime.end.p0(ptr nonnull %3) #5
  br label %osl_spirv_allocate_closure_component.exit.i

75:                                               ; preds = %osl_spirv_closure_param.exit15.i
  %76 = add nsw i32 %68, 16
  %77 = add nsw i32 %68, 1
  %78 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 0
  store i32 %76, ptr %78, align 4, !tbaa !1
  %79 = icmp ne i32 %77, 0
  br i1 %79, label %80, label %Flow4

80:                                               ; preds = %75
  %81 = zext i32 %77 to i64
  %82 = zext i32 %68 to i64
  %83 = trunc i64 %82 to i32
  %84 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %83
  store i32 3, ptr %84, align 4, !tbaa !1
  %85 = trunc i64 %81 to i32
  %86 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %85
  store i32 1065353216, ptr %86, align 4
  %87 = add nsw i32 %68, 2
  %88 = zext i32 %87 to i64
  %89 = trunc i64 %88 to i32
  %90 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %89
  store i32 1065353216, ptr %90, align 4
  %91 = add nsw i32 %68, 3
  %92 = zext i32 %91 to i64
  %93 = trunc i64 %92 to i32
  %94 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %93
  store i32 1065353216, ptr %94, align 4
  %95 = add nsw i32 %68, 4
  %96 = zext i32 %95 to i64
  %97 = trunc i64 %96 to i32
  %98 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %97
  store i32 0, ptr %98, align 4, !tbaa !1
  %99 = add nsw i32 %68, 5
  %100 = zext i32 %99 to i64
  %101 = trunc i64 %100 to i32
  %102 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %101
  store i32 0, ptr %102, align 4, !tbaa !1
  %103 = add nsw i32 %68, 6
  %104 = zext i32 %103 to i64
  %105 = trunc i64 %104 to i32
  %106 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %105
  store i32 0, ptr %106, align 4, !tbaa !1
  %107 = add nsw i32 %68, 7
  %108 = zext i32 %107 to i64
  %109 = trunc i64 %108 to i32
  %110 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %109
  store i32 0, ptr %110, align 4, !tbaa !1
  %111 = add nsw i32 %68, 8
  %112 = zext i32 %111 to i64
  %113 = trunc i64 %112 to i32
  %114 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %113
  store i32 0, ptr %114, align 4, !tbaa !1
  %115 = add nsw i32 %68, 9
  %116 = zext i32 %115 to i64
  %117 = trunc i64 %116 to i32
  %118 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %117
  store i32 0, ptr %118, align 4, !tbaa !1
  %119 = add nsw i32 %68, 10
  %120 = zext i32 %119 to i64
  %121 = trunc i64 %120 to i32
  %122 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %121
  store i32 0, ptr %122, align 4, !tbaa !1
  %123 = add nsw i32 %68, 11
  %124 = zext i32 %123 to i64
  %125 = trunc i64 %124 to i32
  %126 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %125
  store i32 0, ptr %126, align 4, !tbaa !1
  %127 = add nsw i32 %68, 12
  %128 = zext i32 %127 to i64
  %129 = trunc i64 %128 to i32
  %130 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %129
  store i32 0, ptr %130, align 4, !tbaa !1
  %131 = add nsw i32 %68, 13
  %132 = zext i32 %131 to i64
  %133 = trunc i64 %132 to i32
  %134 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %133
  store i32 0, ptr %134, align 4, !tbaa !1
  %135 = add nsw i32 %68, 14
  %136 = zext i32 %135 to i64
  %137 = trunc i64 %136 to i32
  %138 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %137
  store i32 0, ptr %138, align 4, !tbaa !1
  %139 = add nsw i32 %68, 15
  %140 = zext i32 %139 to i64
  %141 = trunc i64 %140 to i32
  %142 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %141
  store i32 0, ptr %142, align 4, !tbaa !1
  br label %Flow4

Flow4:                                            ; preds = %80, %75
  %143 = phi i64 [ %81, %80 ], [ 0, %75 ]
  br label %Flow6

osl_spirv_allocate_closure_component.exit.i:      ; preds = %72, %Flow6
  %144 = phi i64 [ %70, %Flow6 ], [ 0, %72 ]
  %145 = icmp eq i64 %144, 0
  %.inv = xor i1 %145, true
  br i1 %.inv, label %osl_spirv_closure_param.exit14.i, label %osl_spirv_closure_param.exit.i

osl_spirv_closure_param.exit14.i:                 ; preds = %osl_spirv_allocate_closure_component.exit.i
  %146 = add nuw nsw i64 %144, 3
  %147 = and i64 %146, 4294967295
  %148 = trunc i64 %147 to i32
  %149 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %148
  store i32 1559880255, ptr %149, align 4
  %150 = add nuw nsw i64 %144, 4
  %151 = and i64 %150, 4294967295
  %152 = trunc i64 %151 to i32
  %153 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %152
  store i32 -1674806548, ptr %153, align 4
  %154 = add nuw nsw i64 %144, 5
  %155 = and i64 %154, 4294967295
  %156 = getelementptr inbounds %ShaderGlobals, ptr %0, i32 0, i32 3, i32 0
  %157 = load float, ptr %156, align 4
  %158 = bitcast float %157 to i32
  %159 = trunc i64 %155 to i32
  %160 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %159
  store i32 %158, ptr %160, align 4
  %161 = add nuw nsw i64 %144, 6
  %162 = and i64 %161, 4294967295
  %163 = getelementptr inbounds %ShaderGlobals, ptr %0, i32 0, i32 3, i32 1
  %164 = load float, ptr %163, align 4
  %165 = bitcast float %164 to i32
  %166 = trunc i64 %162 to i32
  %167 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %166
  store i32 %165, ptr %167, align 4
  %168 = add nuw nsw i64 %144, 7
  %169 = and i64 %168, 4294967295
  %170 = getelementptr inbounds %ShaderGlobals, ptr %0, i32 0, i32 3, i32 2
  %171 = load float, ptr %170, align 4
  %172 = bitcast float %171 to i32
  %173 = trunc i64 %169 to i32
  %174 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %173
  store i32 %172, ptr %174, align 4
  %175 = add nuw nsw i64 %144, 8
  %176 = and i64 %175, 4294967295
  %177 = trunc i64 %176 to i32
  %178 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %177
  store i32 0, ptr %178, align 4
  %179 = add nuw nsw i64 %144, 9
  %180 = and i64 %179, 4294967295
  %181 = trunc i64 %180 to i32
  %182 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %181
  store i32 0, ptr %182, align 4
  %183 = add nuw nsw i64 %144, 10
  %184 = and i64 %183, 4294967295
  %185 = trunc i64 %184 to i32
  %186 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %185
  store i32 0, ptr %186, align 4
  %187 = add nuw nsw i64 %144, 11
  %188 = and i64 %187, 4294967295
  %189 = trunc i64 %188 to i32
  %190 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %189
  store i32 0, ptr %190, align 4
  %191 = add nuw nsw i64 %144, 12
  %192 = and i64 %191, 4294967295
  %193 = trunc i64 %192 to i32
  %194 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %193
  store i32 0, ptr %194, align 4
  %195 = add nuw nsw i64 %144, 13
  %196 = and i64 %195, 4294967295
  %197 = trunc i64 %196 to i32
  %198 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %197
  store i32 1069547520, ptr %198, align 4
  %199 = add nuw nsw i64 %144, 14
  %200 = and i64 %199, 4294967295
  %201 = trunc i64 %200 to i32
  %202 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %201
  store i32 0, ptr %202, align 4
  br label %osl_spirv_closure_param.exit.i

osl_spirv_closure_param.exit.i:                   ; preds = %osl_spirv_closure_param.exit14.i, %osl_spirv_allocate_closure_component.exit.i
  %brmerge = or i1 %31, %145
  %brmerge.inv = xor i1 %brmerge, true
  %.mux = select i1 %31, i64 %144, i64 %47
  br i1 %brmerge.inv, label %203, label %osl_spirv_add_closure_closure.exit16.i

203:                                              ; preds = %osl_spirv_closure_param.exit.i
  %204 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 0
  %205 = load i32, ptr %204, align 4, !tbaa !1
  %206 = icmp sle i32 %205, 29
  br i1 %206, label %212, label %Flow1

Flow1:                                            ; preds = %Flow, %203
  %207 = phi i64 [ %231, %Flow ], [ poison, %203 ]
  %208 = phi i1 [ false, %Flow ], [ true, %203 ]
  br i1 %208, label %209, label %Flow2

209:                                              ; preds = %Flow1
  call spir_func void @llvm.lifetime.start.p0(ptr nonnull %2) #5
  %210 = getelementptr inbounds %ShaderGlobals, ptr %0, i32 0, i32 19
  %211 = load i32, ptr %210, align 4, !tbaa !9
  call spir_func void @osl_spirv_message(i32 noundef 0, i32 noundef %211, i64 noundef -2300610049087590229, i64 noundef 0, i32 noundef 0, ptr noundef nonnull %2) #5
  call spir_func void @llvm.lifetime.end.p0(ptr nonnull %2) #5
  br label %Flow2

212:                                              ; preds = %203
  %213 = add nsw i32 %205, 3
  %214 = add nsw i32 %205, 1
  %215 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 0
  store i32 %213, ptr %215, align 4, !tbaa !1
  %216 = icmp ne i32 %214, 0
  br i1 %216, label %217, label %Flow

217:                                              ; preds = %212
  %218 = zext i32 %214 to i64
  %219 = add nuw nsw i64 %218, 4294967295
  %220 = and i64 %219, 4294967295
  %221 = trunc i64 %220 to i32
  %222 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %221
  store i32 -2, ptr %222, align 4, !tbaa !1
  %223 = trunc nuw nsw i64 %47 to i32
  %224 = trunc i64 %218 to i32
  %225 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %224
  store i32 %223, ptr %225, align 4, !tbaa !1
  %226 = trunc nuw i64 %144 to i32
  %227 = add nuw nsw i64 %218, 1
  %228 = and i64 %227, 4294967295
  %229 = trunc i64 %228 to i32
  %230 = getelementptr inbounds %Groupdata, ptr %1, i32 0, i32 1, i32 1, i32 %229
  store i32 %226, ptr %230, align 4, !tbaa !1
  br label %Flow

Flow:                                             ; preds = %217, %212
  %231 = phi i64 [ %218, %217 ], [ 0, %212 ]
  br label %Flow1

Flow2:                                            ; preds = %209, %Flow1
  %232 = phi i64 [ 0, %209 ], [ %207, %Flow1 ]
  br label %osl_spirv_add_closure_closure.exit16.i

osl_spirv_add_closure_closure.exit16.i:           ; preds = %osl_spirv_closure_param.exit.i, %Flow2
  %233 = phi i64 [ %.mux, %osl_spirv_closure_param.exit.i ], [ %232, %Flow2 ]
  %234 = getelementptr inbounds %ShaderGlobals, ptr %0, i32 0, i32 23
  store i64 %233, ptr %234, align 8
  ret void
}

; Function Attrs: nocallback nofree nounwind willreturn memory(argmem: write)
declare void @llvm.memset.p0.i64(ptr writeonly captures(none), i8, i64, i1 immarg) #4

attributes #0 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }
attributes #1 = { nocallback nofree nosync nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { "frame-pointer"="non-leaf-no-reserve" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" }
attributes #3 = { nounwind "prefer-vector-width"="128" }
attributes #4 = { nocallback nofree nounwind willreturn memory(argmem: write) }
attributes #5 = { nounwind }

!llvm.ident = !{!0}
!llvm.errno.tbaa = !{!1}
!llvm.module.flags = !{!5, !6, !7, !8}

!0 = !{!"Homebrew clang version 22.1.8"}
!1 = !{!2, !2, i64 0}
!2 = !{!"int", !3, i64 0}
!3 = !{!"omnipotent char", !4, i64 0}
!4 = !{!"Simple C++ TBAA"}
!5 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 5]}
!6 = !{i32 1, !"wchar_size", i32 4}
!7 = !{i32 8, !"PIC Level", i32 2}
!8 = !{i32 7, !"frame-pointer", i32 4}
!9 = !{!10, !2, i64 260}
!10 = !{!"_ZTSN3OSL7v1_16_013ShaderGlobalsE", !11, i64 0, !11, i64 12, !11, i64 24, !11, i64 36, !11, i64 48, !11, i64 60, !11, i64 72, !11, i64 84, !11, i64 96, !12, i64 108, !12, i64 112, !12, i64 116, !12, i64 120, !12, i64 124, !12, i64 128, !11, i64 132, !11, i64 144, !12, i64 156, !12, i64 160, !11, i64 164, !11, i64 176, !11, i64 188, !11, i64 200, !13, i64 216, !13, i64 224, !13, i64 232, !14, i64 240, !13, i64 248, !2, i64 256, !2, i64 260, !15, i64 264, !13, i64 272, !13, i64 280, !16, i64 288, !12, i64 296, !2, i64 300, !2, i64 304, !2, i64 308}
!11 = !{!"_ZTSN9Imath_3_24Vec3IfEE", !12, i64 0, !12, i64 4, !12, i64 8}
!12 = !{!"float", !3, i64 0}
!13 = !{!"any pointer", !3, i64 0}
!14 = !{!"p1 _ZTSN3OSL7v1_16_014ShadingContextE", !13, i64 0}
!15 = !{!"p1 _ZTSN3OSL7v1_16_016RendererServicesE", !13, i64 0}
!16 = !{!"p1 _ZTSN3OSL7v1_16_012ClosureColorE", !13, i64 0}
