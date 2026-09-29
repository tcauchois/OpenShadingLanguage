// Slang 2026.8 (Vulkan SDK 1.4.350.1): slangc segfaults compiling this:
//   slangc switch_crash.slang -target spirv -incomplete-library -o out.spv
// It compiles if the switch is replaced by an if, or if main() doesn't call
// the [__extern] function.
[__extern] void ext(inout uint x);

struct Sum
{
    float result;
    [mutating] void visit(uint id, float w)
    {
        switch (id) {
        case 2: result += w; break;
        default: break;
        }
    }
}

void walk(uint root, inout Sum v)
{
    uint handle = root;
    [ForceUnroll] for (int i = 0; i < 4; ++i) {
        v.visit(handle, float(i));
        handle = handle / 2;
    }
}

[shader("fragment")]
float4 main(float4 pos : SV_Position) : SV_Target
{
    uint x = uint(pos.x);
    ext(x);
    Sum s;
    s.result = 0;
    walk(x, s);
    return float4(s.result, 0, 0, 1);
}
