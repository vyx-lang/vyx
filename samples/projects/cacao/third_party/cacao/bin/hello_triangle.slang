// ============================================================
// Hello Triangle 着色器 (Slang)
//
// 最简单的光栅化着色器：顶点着色器将 2D 位置直接映射到 NDC，
// 片段着色器输出插值后的顶点颜色。
// 展示 Cacao 框架下 Slang 着色器的基本结构。
// ============================================================

// 顶点着色器输入：从顶点缓冲区读取
// 语义 POSITION/COLOR 需要与 C++ 端 AddVertexAttribute 的 semanticName 匹配
struct VSInput
{
    float2 position : POSITION;  // 2D 位置（NDC 空间，范围 -1 到 1）
    float3 color    : COLOR;     // RGB 顶点颜色
};

// 顶点着色器输出 / 片段着色器输入
// SV_Position 是系统语义，GPU 用它做光栅化
// COLOR 语义会在三角形内部自动插值
struct VSOutput
{
    float4 position : SV_Position;  // 裁剪空间位置（GPU 自动做透视除法）
    float3 color    : COLOR;        // 传递给片段着色器的颜色（会被插值）
};

// [shader("vertex")] 标记这是顶点着色器入口
// 每个顶点调用一次，将顶点数据变换到裁剪空间
[shader("vertex")]
VSOutput mainVS(VSInput input)
{
    VSOutput output;
    // 2D 位置扩展为 4D：z=0（平面三角形），w=1（齐次坐标）
    output.position = float4(input.position, 0.0, 1.0);
    // 颜色直接传递，光栅化阶段会自动在三个顶点间插值
    output.color = input.color;
    return output;
}

// [shader("fragment")] 标记这是片段（像素）着色器入口
// 每个像素调用一次，输出最终颜色到渲染目标
[shader("fragment")]
float4 mainPS(VSOutput input) : SV_Target
{
    // 直接使用插值后的颜色，alpha 设为 1.0（完全不透明）
    return float4(input.color, 1.0);
}
