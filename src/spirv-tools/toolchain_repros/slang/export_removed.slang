// An `export` function that the shader doesn't call is removed, and a module
// with only exported functions (and no entry point) is rejected.
//   slangc export_removed.slang -target spirv -incomplete-library -o out.spv
//   spirv-dis out.spv | grep provided_here    (nothing)
RWStructuredBuffer<float> buf;

[noinline] export float provided_here(float x) { return x * 2; }

[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    buf[0] = 1;
}
