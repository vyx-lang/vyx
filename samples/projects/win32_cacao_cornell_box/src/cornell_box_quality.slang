// Cornell Box Path Tracer (Slang)

static const float PI = 3.14159265358979323846;
static const int MAX_BOUNCES = 5;
static const int SAMPLES_PER_FRAME = 8;
static const int DIRECT_LIGHT_SAMPLES = 16;
static const float3 LIGHT_EMISSION = float3(34.0, 24.0, 8.0);

struct CameraParams
{
    float4x4 invView;
    float4x4 invProj;
    uint width;
    uint height;
    uint frameIndex;
    uint _pad;
};

struct Vertex
{
    float3 position;
    float3 normal;
};

[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene : register(t0, space0);
[[vk::binding(1, 0)]] RWTexture2D<float4> outputImage : register(u1, space0);
[[vk::binding(2, 0)]] ConstantBuffer<CameraParams> camera : register(b2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<Vertex> vertices : register(t3, space0);
[[vk::binding(4, 0)]] StructuredBuffer<uint> indices : register(t4, space0);
[[vk::binding(5, 0)]] StructuredBuffer<float> colorData : register(t5, space0);

struct RayPayload
{
    float3 albedo;
    float hitT;
    float3 normal;
    uint materialFlags;
};

uint pcgHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct RNG
{
    uint state;
};

RNG createRNG(uint2 pixel, uint frame)
{
    RNG rng;
    rng.state = pcgHash(pixel.x + pixel.y * 16384u + frame * 268435456u);
    rng.state = pcgHash(rng.state);
    return rng;
}

float nextFloat(inout RNG rng)
{
    rng.state = pcgHash(rng.state);
    return float(rng.state) / 4294967296.0;
}

float3 sampleCosineHemisphere(float3 n, inout RNG rng)
{
    float u1 = nextFloat(rng);
    float u2 = nextFloat(rng);
    float r = sqrt(u1);
    float theta = 2.0 * PI * u2;
    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(max(0.0, 1.0 - u1));

    float3 up = abs(n.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);
    return normalize(t * x + b * y + n * z);
}

float3 sampleAreaLight(inout RNG rng, out float3 lightNormal, out float lightArea)
{
    float u = nextFloat(rng);
    float v = nextFloat(rng);

    float3 p0 = float3(3.43, 5.486, 2.27);
    float3 p1 = float3(3.43, 5.486, 3.32);
    float3 p2 = float3(2.13, 5.486, 2.27);

    float3 point = p0 + (p1 - p0) * u + (p2 - p0) * v;
    lightNormal = float3(0, -1, 0);
    lightArea = length(cross(p1 - p0, p2 - p0));
    return point;
}

float3 GetTriangleNormal(uint triIdx, float2 bary)
{
    uint i0 = indices[triIdx * 3 + 0];
    uint i1 = indices[triIdx * 3 + 1];
    uint i2 = indices[triIdx * 3 + 2];
    float3 n0 = vertices[i0].normal;
    float3 n1 = vertices[i1].normal;
    float3 n2 = vertices[i2].normal;
    return normalize(n0 * (1.0 - bary.x - bary.y) + n1 * bary.x + n2 * bary.y);
}

float3 GetTriangleColor(uint triIdx)
{
    uint quadIdx = triIdx / 2;
    uint base = quadIdx * 12;
    return float3(colorData[base], colorData[base + 1], colorData[base + 2]);
}

bool IsLightTriangle(uint triIdx)
{
    return triIdx >= 30;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    uint triIdx = PrimitiveIndex();
    float2 bary = attribs.barycentrics;

    payload.hitT = RayTCurrent();
    payload.normal = GetTriangleNormal(triIdx, bary);
    payload.albedo = GetTriangleColor(triIdx);
    payload.materialFlags = IsLightTriangle(triIdx) ? 1u : 0u;
}

[shader("miss")]
void Miss(inout RayPayload payload)
{
    payload.albedo = float3(0, 0, 0);
    payload.hitT = -1.0;
    payload.normal = float3(0, 0, 0);
    payload.materialFlags = 0;
}

float3 tracePath(float3 origin, float3 direction, inout RNG rng)
{
    float3 throughput = float3(1, 1, 1);
    float3 radiance = float3(0, 0, 0);

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    for (int bounce = 0; bounce <= MAX_BOUNCES; bounce++)
    {
        RayPayload payload;
        payload.hitT = -1.0;
        payload.materialFlags = 0;

        TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 0, 0, ray, payload);

        if (payload.hitT < 0.0)
            break;

        float3 hitPos = ray.Origin + ray.Direction * payload.hitT;
        float3 hitNormal = payload.normal;
        float3 albedo = payload.albedo;

        if (dot(hitNormal, ray.Direction) > 0)
            hitNormal = -hitNormal;

        if ((payload.materialFlags & 1u) != 0u)
        {
            if (bounce == 0)
                radiance += throughput * LIGHT_EMISSION;
            break;
        }

        float3 directRadiance = float3(0, 0, 0);
        for (int lightSample = 0; lightSample < DIRECT_LIGHT_SAMPLES; lightSample++)
        {
            float3 lightPos, lightNormal;
            float lightArea;
            lightPos = sampleAreaLight(rng, lightNormal, lightArea);

            float3 toLight = lightPos - hitPos;
            float lightDist = length(toLight);
            float3 lightDir = toLight / lightDist;
            float NdotL = dot(hitNormal, lightDir);
            float lightCos = dot(lightNormal, -lightDir);

            if (NdotL > 0.0 && lightCos > 0.0)
            {
                RayDesc shadowRay;
                shadowRay.Origin = hitPos + hitNormal * 0.002;
                shadowRay.Direction = lightDir;
                shadowRay.TMin = 0.001;
                shadowRay.TMax = lightDist - 0.01;

                RayPayload shadowPL;
                shadowPL.hitT = 1.0;
                shadowPL.materialFlags = 0;

                TraceRay(scene,
                         RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE,
                         0xFF, 0, 0, 0, shadowRay, shadowPL);

                if (shadowPL.hitT < 0.0)
                {
                    float solidAngle = lightArea * lightCos / (lightDist * lightDist);
                    directRadiance += throughput * (albedo / PI) * LIGHT_EMISSION * NdotL * solidAngle;
                }
            }
        }
        radiance += directRadiance / float(DIRECT_LIGHT_SAMPLES);

        if (bounce >= 2)
        {
            float q = max(0.05, 1.0 - max(throughput.x, max(throughput.y, throughput.z)));
            if (nextFloat(rng) < q)
                break;
            throughput /= (1.0 - q);
        }

        float3 bounceDir = sampleCosineHemisphere(hitNormal, rng);
        throughput *= albedo;

        ray.Origin = hitPos + hitNormal * 0.002;
        ray.Direction = bounceDir;
        ray.TMin = 0.001;
        ray.TMax = 10000.0;
    }

    float lum = dot(radiance, float3(0.2126, 0.7152, 0.0722));
    if (lum > 10.0)
        radiance *= 10.0 / lum;

    return radiance;
}

[shader("raygeneration")]
void RayGen()
{
    uint2 launchIdx = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;

    float3 radiance = float3(0, 0, 0);

    for (uint sampleIndex = 0; sampleIndex < SAMPLES_PER_FRAME; sampleIndex++)
    {
        RNG rng = createRNG(launchIdx, camera.frameIndex * SAMPLES_PER_FRAME + sampleIndex);

        float2 jitter = float2(nextFloat(rng), nextFloat(rng));
        float2 uv = (float2(launchIdx) + jitter) / float2(launchDim);
        float2 ndc = uv * 2.0 - 1.0;
        ndc.y = -ndc.y;

        float4 clipPos = float4(ndc, 0.0, 1.0);
        float4 viewPos = mul(clipPos, camera.invProj);
        viewPos /= viewPos.w;
        float4 worldDir = mul(float4(viewPos.xyz, 0.0), camera.invView);
        float3 origin = mul(float4(0, 0, 0, 1), camera.invView).xyz;
        float3 direction = normalize(worldDir.xyz);

        radiance += tracePath(origin, direction, rng);
    }

    radiance /= float(SAMPLES_PER_FRAME);

    if (camera.frameIndex > 0)
    {
        float3 prev = outputImage[launchIdx].xyz;
        prev = pow(max(prev, float3(0.001, 0.001, 0.001)), 2.2);
        prev = -log(max(float3(1, 1, 1) - prev, float3(0.001, 0.001, 0.001))) / 1.0;
        float weight = 1.0 / float(camera.frameIndex + 1);
        radiance = lerp(prev, radiance, weight);
    }

    float3 mapped = float3(1, 1, 1) - exp(-radiance * 1.0);
    float3 gammaCorrected = pow(max(mapped, float3(0, 0, 0)), 1.0 / 2.2);

    outputImage[launchIdx] = float4(gammaCorrected, 1.0);
}
