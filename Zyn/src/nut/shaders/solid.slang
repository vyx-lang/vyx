struct SolidVSInput
{
    float2 position : POSITION;
    float4 color : COLOR;
};

struct SolidVSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

[shader("vertex")]
SolidVSOutput SolidVS(SolidVSInput input)
{
    SolidVSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 SolidPS(SolidVSOutput input) : SV_Target
{
    return input.color;
}

struct TexturedVSInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float layer : TEXCOORD1;
    float4 color : COLOR;
};

struct TexturedVSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float layer : TEXCOORD1;
    float4 color : COLOR;
};

Texture2DArray<float4> SpriteTexture : register(t0);
SamplerState SpriteSampler : register(s1);

[shader("vertex")]
TexturedVSOutput TexturedVS(TexturedVSInput input)
{
    TexturedVSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.uv = input.uv;
    output.layer = input.layer;
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 TexturedPS(TexturedVSOutput input) : SV_Target
{
    return SpriteTexture.Sample(SpriteSampler, float3(input.uv, input.layer)) * input.color;
}
