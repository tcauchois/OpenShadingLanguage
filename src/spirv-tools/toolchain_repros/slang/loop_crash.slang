// slangc segfaults: a shader that calls an [__extern] function, plus an
// exported function (kept for linking) that has a loop that isn't unrolled.
// It compiles with [ForceUnroll] on the loop.
//   slangc loop_crash.slang -target spirv -incomplete-library -o out.spv
[__extern] void ext(inout uint x);

RWStructuredBuffer<uint> buf;

[noinline] export uint count_words(uint n)
{
    uint total = 0;
    for (uint i = 0; i < 64; ++i)
        if (i < n)
            total += buf[i];
    return total;
}

[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint x = tid.x;
    ext(x);
    if (x == 0xffffffffu)
        buf[0] = count_words(x);  // keeps count_words
}
