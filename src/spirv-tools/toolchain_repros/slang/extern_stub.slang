// [__extern] functions are emitted as bodiless OpFunctions with no
// LinkageAttributes Import decoration (and no Linkage capability), so the
// module can't be linked against one that defines them. And an `export`
// function gets no Export decoration either.
//   slangc extern_stub.slang -target spirv -incomplete-library -o out.spv
//   spirv-dis out.spv | grep -E "Linkage|OpFunction |OpName"
[__extern] float provided_elsewhere(float x);

RWStructuredBuffer<float> buf;

[noinline] export float provided_here(float x) { return x * 2; }

[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    buf[0] = provided_elsewhere(buf[1]) + provided_here(buf[2]);
}
