// Astra (the Astra companion's avatar) in Death Stranding. The Astra add-on uploads the engine's newest picture of her
// (ASTRAWORLD: premultiplied colour, ASTRADEPTH: depth, flags in AstraPlanes) and the camera it was drawn from; this
// re-projects it onto DS's current camera, hides her behind DS's surfaces and Minecraft's blocks (MCPassthrough.fx's
// McInfoTex, when that effect runs first), matches DS's light a little and lays DS's UI (DSUI) back over her.
// Always enabled (annotation): AstraActive gates it, so DS passes through untouched while she isn't out.
#include "ReShade.fxh"

// The part of DS's depth buffer the scene was drawn into (set by the add-on: under upscaling, e.g. FSR in
// Death Stranding 2, the scene is rendered smaller into the top-left of a full-size buffer); 1 at native resolution
uniform float2 DepthUvScale = float2(1.0, 1.0);

texture AstraWorldTex : ASTRAWORLD;
texture AstraDepthTex : ASTRADEPTH;
sampler sAstraWorld { Texture = AstraWorldTex; AddressU = CLAMP; AddressV = CLAMP; };
sampler sAstraDepth { Texture = AstraDepthTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
// her depth seen from the sun (the engine's light view): metres along SunVDir from its near plane, 0 where she isn't
texture AstraSunTex : ASTRASUN;
sampler sAstraSun { Texture = AstraSunTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
// DS's own UI target (bound by the add-ons every frame)
texture DsUiTex : DSUI;
sampler sDsUi { Texture = DsUiTex; AddressU = CLAMP; AddressV = CLAMP; };
// Minecraft's coverage (x) and depth (y) per pixel, written by MCPassthrough.fx (the same texture: same name)
texture McInfoTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler sMcInfo { Texture = McInfoTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
// DS's fog, read off its own picture (dark channel prior): per cell of a coarse grid, an upper bound of the fog's
// density from how washed out the darkest detail is at that depth, and the haziest colours (the fog's colour)
#define FOG_W 64
#define FOG_H 36
texture FogSigmaTex { Width = FOG_W; Height = FOG_H; Format = R32F; };
sampler sFogSigma { Texture = FogSigmaTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
texture FogColourTex { Width = FOG_W; Height = FOG_H; Format = RGBA16F; MipLevels = 7; };
sampler sFogColour { Texture = FogColourTex; AddressU = CLAMP; AddressV = CLAMP; };
// x: density (1/m), yzw: the fog's colour; smoothed over frames (FogPrevTex: last frame's)
texture FogParamsTex { Width = 1; Height = 1; Format = RGBA16F; };
sampler sFogParams { Texture = FogParamsTex; MinFilter = POINT; MagFilter = POINT; };
texture FogPrevTex { Width = 1; Height = 1; Format = RGBA16F; };
sampler sFogPrev { Texture = FogPrevTex; MinFilter = POINT; MagFilter = POINT; };

// DS's picture at quarter size with mips: the light around her
texture AstraLightTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA8; MipLevels = 7; };
sampler sAstraLight { Texture = AstraLightTex; AddressU = CLAMP; AddressV = CLAMP; };

// set by the add-on
uniform bool AstraActive = false;
uniform bool McPresent = false; // Minecraft's effect composited this frame: its McInfoTex is current
// near, far, flags (1: [0,1] depth, 2: rows bottom-up, 4: reversed Z, 8: depth is linear metres along the view axis)
uniform float3 AstraPlanes = float3(0.05, 300.0, 24.0);
uniform float2 HostPlanes = float2(0.2, 4000.0);
// rows of R_her^T R_ds (DS camera ray -> her picture's camera ray), DS's camera in her picture's camera space,
// x = tan(DS vertical fov / 2), y = tan(her picture's vertical fov / 2), z = her picture's aspect
uniform float3 AstraWarpRow0 = float3(1.0, 0.0, 0.0);
uniform float3 AstraWarpRow1 = float3(0.0, 1.0, 0.0);
uniform float3 AstraWarpRow2 = float3(0.0, 0.0, 1.0);
uniform float3 AstraWarpT = float3(0.0, 0.0, 0.0);
uniform bool AstraWarpIdentity = false; // her picture is of this very camera: no re-projection
uniform float3 AstraWarpTan = float3(0.42, 0.42, 1.7778);
uniform bool DsUiActive = false;
// her shadow: toward the sun and her feet in DS's camera space, and how dark it may be (0: no sun on her)
uniform float3 SunView = float3(0.0, 1.0, 0.0);
uniform float3 AstraFeetView = float3(0.0, 0.0, -5.0);
uniform float3 UpView = float3(0.0, 1.0, 0.0); // the world's up in DS's camera space
uniform float AstraHeight = 1.68; // her height (m, with her size)
// the sun view, in DS's camera space: centre, right (+u), up (+v), the way the light travels; half width, near, N
uniform bool SunVActive = false;
uniform float3 SunVCenter = float3(0.0, 0.0, -5.0);
uniform float3 SunVRight = float3(1.0, 0.0, 0.0);
uniform float3 SunVUp = float3(0.0, 1.0, 0.0);
uniform float3 SunVDir = float3(0.0, -1.0, 0.0);
uniform float3 SunVParams = float3(1.0, 0.0, 512.0);
uniform float AstraSize = 1.2;
uniform float AstraShadow = 0.0;

uniform float DepthBias < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.005; ui_label = "Depth bias (m)";
	ui_tooltip = "How far behind DS's surface she may still show (her feet on the ground)."; > = 0.04;
uniform float SlopeBias < ui_type = "drag"; ui_min = 0.0; ui_max = 40.0; ui_step = 0.1; ui_label = "Depth bias per grazing slope"; > = 0.0;
uniform float MaxBias < ui_type = "drag"; ui_min = 0.1; ui_max = 10.0; ui_step = 0.1; ui_label = "Largest depth bias (m)"; > = 3.0;
uniform float LightMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match DS lighting";
	ui_tooltip = "Darken/tint her by DS's light around her. She is already lit (MToon) by her engine, so only a little."; > = 0.3;
uniform float LightReference < ui_type = "drag"; ui_min = 0.1; ui_max = 1.0; ui_step = 0.01; ui_label = "Neutral brightness"; > = 0.42;
uniform float LightTint < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Light colour"; > = 0.55;
uniform float LightBlur < ui_type = "drag"; ui_min = 1.0; ui_max = 6.0; ui_step = 0.1; ui_label = "Light blur (mip)"; > = 3.2;
uniform float GradeMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match DS colour grade"; > = 0.25;
uniform float HazeStart < ui_type = "drag"; ui_min = 0.0; ui_max = 200.0; ui_step = 1.0; ui_label = "Haze start (m)"; > = 15.0;
uniform float HazeDistance < ui_type = "drag"; ui_min = 10.0; ui_max = 1000.0; ui_step = 1.0; ui_label = "Haze distance (m)"; > = 160.0;
uniform float HazeStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Haze strength (fixed haze, when DS's fog isn't measured)"; > = 0.0;
uniform float FogMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01; ui_label = "Match DS's fog";
	ui_tooltip = "Her fog by DS's own: its colour and density measured from DS's picture every frame (1: as measured)."; > = 1.0;
uniform float DsUiGamma < ui_type = "drag"; ui_min = 0.2; ui_max = 3.0; ui_step = 0.01; ui_label = "DS UI gamma"; > = 1.0;
uniform float DsUiStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01; ui_label = "DS UI brightness"; > = 1.0;
uniform float ShadowStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Her shadow";
	ui_tooltip = "How dark her shadow on DS's ground is in full sun."; > = 0.5;
uniform float ShadowReach < ui_type = "drag"; ui_min = 1.0; ui_max = 20.0; ui_step = 0.1; ui_label = "Shadow length limit (m)"; > = 10.0;
uniform float ReprojectThickness < ui_type = "drag"; ui_min = 0.05; ui_max = 3.0; ui_step = 0.01; ui_label = "Her depth for re-projection (m)";
	ui_tooltip = "When her picture is a frame older than the camera, a point this far behind her front still counts as her."; > = 0.35;
uniform int DebugView < ui_type = "combo"; ui_items = "Composite\0Her depth (1 m bands)\0DS depth (1 m bands)\0"; > = 0;

float src_linear(float d, float3 planes)
{
	const int flags = int(planes.z + 0.5);
	const float n = planes.x, f = planes.y;
	if (flags & 8)
		return d > 0.0 ? d : 1e9;
	if (flags & 4)
		return d > 0.0 ? n * f / (n + d * (f - n)) : 1e9;
	return d < 1.0 ? n * f / (f - d * (f - n)) : 1e9;
}

/// Where a source's picture is in its texture: ndc (y up) -> uv, by its row order.
float2 src_uv(float2 ndc, float3 planes)
{
	const bool bottom_up = (int(planes.z + 0.5) & 2) != 0;
	return bottom_up ? ndc * 0.5 + 0.5 : float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

/// Re-projection: march this DS pixel's ray (in DS's current camera) from near to far, moving each point into the
/// camera the source rendered with (rotation + how far the camera moved since). The first point that is behind the
/// source's surface there is where the source is seen: refine it. Thin, close things (Steve, Astra) are found even
/// when the camera has swung sideways. Returns the source's uv and its depth in DS's camera, or false.
bool reproject(float2 uv, float zFar, sampler sd, float3 planes, float3 r0, float3 r1, float3 r2, float3 T, float3 tanv,
	out float2 suv, out float zs)
{
	suv = 0.0;
	zs = 1e9;
	const float3 ray = float3((uv.x * 2.0 - 1.0) * tanv.x * BUFFER_WIDTH * BUFFER_RCP_HEIGHT, (1.0 - uv.y * 2.0) * tanv.x, -1.0);
	const float2 scale = float2(tanv.y * tanv.z, tanv.y);
	const float zNear = 0.2;
	// her picture was drawn from this very camera: no marching, the pixel itself
	if (AstraWarpIdentity)
	{
		const float2 n = ray.xy / scale;
		suv = src_uv(n, planes);
		zs = src_linear(tex2Dlod(sd, float4(suv, 0, 0)).r, planes);
		return zs < 1e8 && zs < zFar + 0.05;
	}
	float zprev = zNear;
	[loop] for (int i = 0; i < 24; ++i)
	{
		const float z = zNear * pow(zFar / zNear, i / 23.0);
		const float3 p = ray * z;
		const float3 pm = float3(dot(r0, p), dot(r1, p), dot(r2, p)) + T;
		if (pm.z > -1e-3)
			continue;
		const float2 n = pm.xy / -pm.z / scale;
		if (any(abs(n) > 1.0))
			continue;
		const float zmv = src_linear(tex2Dlod(sd, float4(src_uv(n, planes), 0, 0)).r, planes);
		if (zmv > -pm.z + 0.03)
		{
			zprev = z;
			continue; // still in front of whatever the source drew there
		}
		// the step went from in front of her to behind: find where (halving), then is it her surface or a gap behind?
		float lo = zprev, hi = z;
		[loop] for (int b = 0; b < 7; ++b)
		{
			const float mid = 0.5 * (lo + hi);
			const float3 pb = ray * mid;
			const float3 mb = float3(dot(r0, pb), dot(r1, pb), dot(r2, pb)) + T;
			const float2 nb = mb.xy / -mb.z / scale;
			const float zb = mb.z < -1e-3 && all(abs(nb) <= 1.0) ? src_linear(tex2Dlod(sd, float4(src_uv(nb, planes), 0, 0)).r, planes) : 1e9;
			if (zb > -mb.z + 0.03)
				lo = mid;
			else
				hi = mid;
		}
		{
			const float3 ph = ray * hi;
			const float3 mh = float3(dot(r0, ph), dot(r1, ph), dot(r2, ph)) + T;
			const float2 nh = mh.xy / -mh.z / scale;
			const float zh2 = mh.z < -1e-3 && all(abs(nh) <= 1.0) ? src_linear(tex2Dlod(sd, float4(src_uv(nh, planes), 0, 0)).r, planes) : 1e9;
			if (zh2 > 1e8 || -mh.z - zh2 > ReprojectThickness)
			{
				zprev = z;
				continue; // the ray passes behind her (taking it would smear her along the motion)
			}
		}
		const float z_hit = hi;
		// crossed the source's surface: settle on it (fixed-point on the surface point's DS depth)
		float zg = z_hit;
		float2 ndc = n;
		{
			const float3 ph = ray * z_hit;
			const float3 mh = float3(dot(r0, ph), dot(r1, ph), dot(r2, ph)) + T;
			ndc = mh.xy / -mh.z / scale;
		}
		[loop] for (int k = 0; k < 3; ++k)
		{
			const float3 pk = ray * zg;
			const float3 mk = float3(dot(r0, pk), dot(r1, pk), dot(r2, pk)) + T;
			if (mk.z > -1e-3)
				break;
			const float2 nk = mk.xy / -mk.z / scale;
			if (any(abs(nk) > 1.0))
				break;
			const float zk = src_linear(tex2Dlod(sd, float4(src_uv(nk, planes), 0, 0)).r, planes);
			if (zk > 1e8)
				break;
			ndc = nk;
			const float3 q = float3(nk * scale, -1.0) * zk - T;
			zg = -(q.x * r0.z + q.y * r1.z + q.z * r2.z);
		}
		suv = src_uv(ndc, planes);
		zs = zg;
		return true;
	}
	return false;
}

float host_linear(float d)
{
	// DS's depth is reversed Z
	const float n = HostPlanes.x, f = HostPlanes.y;
	return d > 0.0 ? n * f / (n + d * (f - n)) : 1e9;
}

float luma(float3 c)
{
	return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float3 bands(float z)
{
	return lerp(float3(0.1, 0.1, 0.1), float3(0.4, 0.85, 1.0), step(0.5, frac(z))) * saturate(1.5 - z / 100.0);
}

/// DS's fog between the camera and a point z metres away: how much of the point still shows (and the fog's colour).
float fog_transmittance(float z, out float3 colour)
{
	const float4 f = tex2Dfetch(sFogParams, int2(0, 0));
	colour = f.yzw;
	return exp(-max(f.x, 0.0) * FogMatch * z);
}

/// Her own shading stays; DS's local light darkens and tints her a little, DS's grade colours her, far away DS's haze.
float3 relight(float3 c, float2 uv, float z)
{
	const float3 L = tex2Dlod(sAstraLight, float4(uv, 0, LightBlur)).rgb;
	const float lum = luma(L);
	const float gain = clamp(lum / LightReference, 0.42, 1.3);
	const float3 tint = clamp(lerp(1.0, L / max(lum, 1e-3), LightTint), 0.55, 1.5);
	c = lerp(c, c * gain * tint, LightMatch);
	const float3 M = tex2Dlod(sAstraLight, float4(0.5, 0.5, 0, 6.0)).rgb;
	const float3 grade = clamp(lerp(1.0, M / max(luma(M), 1e-3), 0.6), 0.6, 1.6);
	c = lerp(c, c * grade, GradeMatch);
	const float h = saturate((1.0 - exp(-max(z - HazeStart, 0.0) / HazeDistance)) * HazeStrength);
	c = lerp(c, tex2Dlod(sAstraLight, float4(uv, 0, LightBlur + 1.0)).rgb, h);
	// DS's fog, as measured on its own picture
	float3 fog;
	const float t = fog_transmittance(z, fog);
	return lerp(fog, c, t);
}

/// Her shadow without a view of her from the sun: her body as a capsule (her height, upright at her feet), and the
/// sun ray from this DS surface point (DS camera space) passing it: the nearer the ray passes her axis, the darker,
/// softer the further the shadow falls. The same from any camera.
float her_shadow(float3 p)
{
	const float3 up = normalize(UpView);
	const float r = 0.17 * AstraSize;
	const float3 a = AstraFeetView + up * r, b = AstraFeetView + up * max(AstraHeight - r, r);
	// closest approach between the ray p + SunView t (t > 0) and the segment a..b
	const float3 d1 = SunView, d2 = b - a, w = p - a;
	const float aa = dot(d1, d1), bb = dot(d1, d2), cc = dot(d2, d2), dd = dot(d1, w), ee = dot(d2, w);
	const float den = aa * cc - bb * bb;
	float s = den > 1e-6 ? (bb * ee - cc * dd) / den : 0.0;
	float u = den > 1e-6 ? (aa * ee - bb * dd) / den : 0.0;
	u = saturate(u);
	s = max((bb * u - dd) / aa, 0.0); // the ray's point nearest to the clamped axis point
	if (s > ShadowReach)
		return 0.0;
	const float dist = length(p + d1 * s - (a + d2 * u));
	const float soft = 0.05 + 0.04 * s;
	return (1.0 - smoothstep(r - soft, r + soft, dist)) * (1.0 - saturate((s / ShadowReach - 0.7) * 3.3));
}

/// Her real shadow: this DS surface point in the engine's view of her from the sun; shadowed where it lies further
/// along the light than her surface there (3x3 taps soften the edge).
float her_shadow_map(float3 p)
{
	const float3 d = p - SunVCenter;
	const float2 uv = float2(dot(d, SunVRight), dot(d, SunVUp)) / SunVParams.x;
	if (any(abs(uv) > 1.0))
		return 0.0;
	const float depth = dot(d, SunVDir) + SunVParams.y;
	const float2 tc = float2(uv.x * 0.5 + 0.5, 0.5 - uv.y * 0.5);
	const float texel = 1.0 / SunVParams.z;
	float shadow = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const float z = tex2Dlod(sAstraSun, float4(tc + float2(x, y) * texel * 1.5, 0, 0)).r;
			shadow += z > 0.0 && depth > z + 0.04 ? 1.0 : 0.0;
		}
	return shadow / 9.0;
}

/// One cell: DS's colour and depth there; its dark channel (darkest channel over a small patch, against the fog colour
/// of last frame) bounds the fog's transmittance from below, so -ln(1 - dark) / depth bounds its density from above.
void PS_FogCells(float4 pos : SV_Position, float2 uv : TEXCOORD, out float outSigma : SV_Target0, out float4 outColour : SV_Target1)
{
	const float4 prev = tex2Dfetch(sFogPrev, int2(0, 0));
	const float3 A = max(prev.yzw, 0.05);
	float dark = 1.0;
	float3 avg = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const float3 c = tex2Dlod(ReShade::BackBuffer, float4(uv + float2(x, y) * float2(0.33 / FOG_W, 0.33 / FOG_H), 0, 0)).rgb;
			dark = min(dark, min(c.r / A.r, min(c.g / A.g, c.b / A.b)));
			avg += c;
		}
	avg /= 9.0;
	const float d = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv * DepthUvScale, 0, 0)).x);
	// usable: not too near (noise), not the sky (no depth: it is the fog's colour itself)
	const bool usable = d > 8.0 && d < 2000.0 && d < HostPlanes.y * 0.9;
	outSigma = usable ? -log(max(1.0 - 0.95 * saturate(dark), 0.02)) / d : 1e4;
	// the haziest places show the fog's colour: weight by how washed out they are (the sky counts fully)
	const float w = usable ? pow(saturate(dark), 8.0) : (d >= HostPlanes.y * 0.9 ? 1.0 : 0.0);
	outColour = float4(avg * w, w);
}

/// The frame's fog: density = the mean of the smallest bounds over blocks of cells (the clearest dark detail in each
/// part of the picture), colour = the weighted mean of the haziest; eased from last frame's.
float4 PS_FogParams(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float sum = 0.0, n = 0.0;
	[loop] for (int by = 0; by < FOG_H / 6; ++by)
		[loop] for (int bx = 0; bx < FOG_W / 8; ++bx)
		{
			float m = 1e4;
			[loop] for (int y = 0; y < 6; ++y)
				[loop] for (int x = 0; x < 8; ++x)
					m = min(m, tex2Dfetch(sFogSigma, int2(bx * 8 + x, by * 6 + y)).r);
			if (m < 1e3)
			{
				sum += m;
				n += 1.0;
			}
		}
	const float sigma = n > 0.0 ? sum / n : 0.0;
	const float4 c = tex2Dlod(sFogColour, float4(0.5, 0.5, 0, 6));
	const float4 prev = tex2Dfetch(sFogPrev, int2(0, 0));
	const float3 colour = c.a > 1e-5 ? c.rgb / c.a : (prev.w > 0.0 ? prev.yzw : float3(0.6, 0.6, 0.6));
	const float k = prev.w > 0.0 ? 0.08 : 1.0; // ease over ~a dozen frames
	return float4(lerp(prev.x, sigma, k), lerp(prev.yzw, colour, k));
}

float4 PS_FogKeep(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return tex2Dfetch(sFogParams, int2(0, 0));
}

float4 PS_Light(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(tex2D(ReShade::BackBuffer, uv).rgb, 1.0);
}

float3 PS_Astra(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float3 host = tex2D(ReShade::BackBuffer, uv).rgb;
	if (!AstraActive)
		return host;
	const float zh = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv * DepthUvScale, 0, 0)).x);
	if (DebugView == 2)
		return bands(zh);
	const float allow = min(DepthBias + SlopeBias * abs(ddy(zh)), max(MaxBias, DepthBias));
	const float zFar = max(min(zh + allow, 400.0), 0.5);
	float2 auv;
	float za = 1e9;
	float4 astra = 0.0;
	if (reproject(uv, zFar, sAstraDepth, AstraPlanes, AstraWarpRow0, AstraWarpRow1, AstraWarpRow2, AstraWarpT, AstraWarpTan, auv, za))
		astra = tex2D(sAstraWorld, auv);
	if (DebugView == 1)
		return astra.a > 0.0 ? bands(za) : host * 0.3;
	// behind DS's surfaces, and behind Minecraft's blocks where they are nearer
	float visible = za < zh + allow ? 1.0 : 0.0;
	if (McPresent)
	{
		const float4 mc = tex2Dlod(sMcInfo, float4(uv, 0, 0));
		if (mc.x > 0.5 && mc.y > 0.0 && mc.y < za - 0.02)
			visible = 0.0;
	}
	const float cover = astra.a * visible;
	// her shadow on DS's surfaces around her (where she doesn't cover them herself)
	float3 shaded = host;
	if (AstraShadow > 0.0 && ShadowStrength > 0.0 && cover < 0.999 && zh < 200.0)
	{
		const float3 p = float3((uv.x * 2.0 - 1.0) * AstraWarpTan.x * BUFFER_WIDTH * BUFFER_RCP_HEIGHT, (1.0 - uv.y * 2.0) * AstraWarpTan.x, -1.0) * zh;
		if (distance(p, AstraFeetView) < ShadowReach + 2.0)
		{
			float3 fogc;
			const float tg = fog_transmittance(zh, fogc); // the fog over that ground fills her shadow in
			shaded = host * (1.0 - ShadowStrength * AstraShadow * tg * (SunVActive ? her_shadow_map(p) : her_shadow(p)));
		}
	}
	// (her colour is premultiplied: relight it as straight colour, then by her coverage again)
	const float3 lit = visible > 0.0 && astra.a > 1e-4 ? relight(astra.rgb / astra.a, uv, za) * astra.a : 0.0;
	float3 c = lit * visible + shaded * (1.0 - cover);
	// DS's UI back over her (DS composited it with its scene before she was drawn over that)
	if (DsUiActive && cover > 0.001)
	{
		const float4 ui = tex2D(sDsUi, uv);
		const float3 ui_rgb = pow(max(ui.rgb, 0.0), DsUiGamma) * DsUiStrength;
		c = lerp(c, ui_rgb + c * saturate(1.0 - ui.a), saturate(cover));
	}
	return saturate(c);
}

technique AstraPassthrough < enabled = true; ui_tooltip = "Astra, drawn by her engine from DS's camera (on while her engine is linked)."; >
{
	pass FogCells
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_FogCells;
		RenderTarget0 = FogSigmaTex;
		RenderTarget1 = FogColourTex;
	}
	pass FogParams
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_FogParams;
		RenderTarget = FogParamsTex;
	}
	pass FogKeep
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_FogKeep;
		RenderTarget = FogPrevTex;
	}
	pass Light
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Light;
		RenderTarget = AstraLightTex;
	}
	pass Astra
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Astra;
	}
}
