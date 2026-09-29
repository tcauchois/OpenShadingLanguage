// Compile time grows very quickly with nested unrolled loops over a struct
// passed by value. (A walker over a tree of nodes in a word pool, which
// calls a visitor through a generic interface, copying each node's words,
// as OSL's closure printing first did.)
//   time slangc slow_unroll.slang -target spirv -incomplete-library -o out.spv
[__extern] void ext(inout uint x);

struct Pool { uint used; uint words[256]; };

interface IVisitor { [mutating] void visit(uint node, Pool pool); }

RWStructuredBuffer<uint> dst;

struct Copier : IVisitor
{
    uint n;
    [mutating] void visit(uint node, Pool pool)
    {
        uint count = pool.words[node];
        [ForceUnroll] for (uint i = 0; i < 128; ++i)
            if (i < count)
                dst[n + i] = pool.words[node + 1 + i];
        n += count;
    }
}

void walk<V : IVisitor>(Pool pool, uint root, inout V v)
{
    uint node = root;
    [ForceUnroll] for (int iter = 0; iter < 64; ++iter) {
        if (node == 0)
            break;
        v.visit(node, pool);
        node = pool.words[node + 129];
    }
}

RWStructuredBuffer<Pool> pools;

[shader("compute")][numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint x = tid.x;
    ext(x);
    Pool pool = pools[0];
    Copier c;
    c.n = 0;
    walk(pool, x, c);
}
