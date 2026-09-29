RWStructuredBuffer<float4> outbuf;
static float4 acc = 0;
[__extern] void eval_osl(float u);
[shader("compute")][numthreads(64,1,1)]
void main(uint3 tid : SV_DispatchThreadID) { eval_osl(tid.x * 0.01); outbuf[tid.x] = acc; }
