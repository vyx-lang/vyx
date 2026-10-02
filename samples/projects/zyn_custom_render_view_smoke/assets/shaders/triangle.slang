struct VertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR;
};

[shader("vertex")]
VertexOutput mainVS(uint vertexID : SV_VertexID) {
    VertexOutput output;
    if (vertexID == 0) {
        output.position = float4(0.0, 0.65, 0.0, 1.0);
        output.color = float3(1.0, 0.27, 0.32);
    } else if (vertexID == 1) {
        output.position = float4(-0.72, -0.60, 0.0, 1.0);
        output.color = float3(0.2, 0.78, 0.9);
    } else {
        output.position = float4(0.72, -0.60, 0.0, 1.0);
        output.color = float3(0.55, 0.36, 1.0);
    }
    return output;
}

[shader("fragment")]
float4 mainPS(VertexOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
