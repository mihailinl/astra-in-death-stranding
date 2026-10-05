// Composites Minecraft (passthrough mod) into GTA V. The MCPassthrough add-on (MCPassthrough.asi) uploads
// Minecraft's latest frame into the MCWORLD / MCDEPTH / MCOVERLAY textures and sets the plane uniforms.
//
// world: premultiplied RGBA, drawn where Minecraft is nearer than GTA's depth buffer (both in metres).
// overlay: Minecraft's hand, HUD and screens, always on top.
#include "ReShade.fxh"

texture MCWorldTex : MCWORLD;
texture MCDepthTex : MCDEPTH;
texture MCOverlayTex : MCOVERLAY;
sampler sWorld { Texture = MCWorldTex; AddressU = CLAMP; AddressV = CLAMP; };
sampler sDepth { Texture = MCDepthTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
sampler sOverlay { Texture = MCOverlayTex; AddressU = CLAMP; AddressV = CLAMP; };
// Astra (the Astra avatar engine's picture of her, rendered from DS's camera): premultiplied colour + depth
texture AstraWorldTex : ASTRAWORLD;
texture AstraDepthTex : ASTRADEPTH;
sampler sAstraWorld { Texture = AstraWorldTex; AddressU = CLAMP; AddressV = CLAMP; };
sampler sAstraDepth { Texture = AstraDepthTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };

// x = near, y = far, z = flags (1: [0,1] depth, 2: rows bottom-up, 4: reversed Z). Set by the add-on.
uniform float3 McPlanes = float3(0.05, 2048.0, 7.0);
// GTA's camera near/far clip. Set by the add-on.
uniform float2 HostPlanes = float2(0.15, 10000.0);

// Set true by the add-on only while it has uploaded a Minecraft frame; until then GTA passes through untouched.
uniform bool McActive = false;
// The same for Astra; her planes: near, far, flags (1: [0,1] depth, 2: rows bottom-up, 4: reversed Z, 8: depth is
// linear metres along the view axis, 16: no overlay layer)
uniform bool AstraActive = false;
uniform float3 AstraPlanes = float3(0.05, 1000.0, 5.0);
uniform float3 AstraWarpRow0 = float3(1.0, 0.0, 0.0);
uniform float3 AstraWarpRow1 = float3(0.0, 1.0, 0.0);
uniform float3 AstraWarpRow2 = float3(0.0, 0.0, 1.0);
uniform float3 AstraWarpTan = float3(0.7, 0.7, 1.7777);
uniform float3 AstraWarpT = float3(0.0, 0.0, 0.0);
uniform float AstraLightMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match DS lighting (Astra)";
	ui_tooltip = "Darken/tint Astra by DS's light around her. She is already lit (MToon) by her engine, so far less than Minecraft."; > = 0.3;
uniform float AstraGradeMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match DS colour grade (Astra)"; > = 0.25;

uniform bool HostReversedZ < ui_label = "GTA depth is reversed"; > = true;
uniform float DepthBias < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.005; ui_label = "Depth bias (m)";
	ui_tooltip = "How far behind GTA's surface Minecraft may still show (blocks resting on the ground)."; > = 0.04;
uniform float SlopeBias < ui_type = "drag"; ui_min = 0.0; ui_max = 40.0; ui_step = 0.1; ui_label = "Depth bias per grazing slope";
	ui_tooltip = "Extra depth bias where GTA's surface is seen at a grazing angle (its depth changes fast down the screen): Minecraft ground laid into GTA's (lava, netherrack) still shows far away."; > = 0.0;
uniform float MaxBias < ui_type = "drag"; ui_min = 0.1; ui_max = 10.0; ui_step = 0.1; ui_label = "Largest depth bias (m)"; > = 3.0;
uniform float HostDepthScale < ui_type = "drag"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.001; ui_label = "GTA depth scale";
	ui_tooltip = "Calibration: multiplies GTA's linearised depth."; > = 1.0;
uniform float LightMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match GTA lighting";
	ui_tooltip = "Relight Minecraft by the (blurred) GTA picture around it: darker in shade, tinted by nearby light."; > = 0.8;
uniform float LightReference < ui_type = "drag"; ui_min = 0.1; ui_max = 1.0; ui_step = 0.01; ui_label = "Neutral brightness";
	ui_tooltip = "GTA brightness at which Minecraft keeps its own colours."; > = 0.42;
uniform float LightTint < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Light colour"; > = 0.55;
uniform float LightBlur < ui_type = "drag"; ui_min = 1.0; ui_max = 6.0; ui_step = 0.1; ui_label = "Light blur (mip)"; > = 3.2;
uniform float GradeMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match GTA colour grade";
	ui_tooltip = "Give Minecraft the colour of GTA's whole picture (a red Nether night, an orange sunset), as GTA's own grading does to its world."; > = 0.55;
uniform float HazeStart < ui_type = "drag"; ui_min = 0.0; ui_max = 200.0; ui_step = 1.0; ui_label = "Haze start (m)"; > = 15.0;
uniform float HazeDistance < ui_type = "drag"; ui_min = 10.0; ui_max = 1000.0; ui_step = 1.0; ui_label = "Haze distance (m)";
	ui_tooltip = "Far away, Minecraft fades into the GTA picture around it (GTA's haze and fog don't reach Minecraft otherwise)."; > = 160.0;
uniform float HazeStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Haze strength"; > = 0.85;
uniform float EdgeSoftness < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Soften Minecraft's edges";
	ui_tooltip = "Anti-alias where Minecraft meets GTA (GTA's own anti-aliasing ran before Minecraft was added)."; > = 1.0;
uniform float ContactShadow < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Contact shadows";
	ui_tooltip = "Darken GTA's surfaces right next to Minecraft's (under mobs' feet, around blocks)."; > = 0.5;
uniform float ContactRadius < ui_type = "drag"; ui_min = 0.1; ui_max = 3.0; ui_step = 0.05; ui_label = "Contact shadow size (m)"; > = 0.7;
uniform float BloomStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01; ui_label = "Minecraft glow";
	ui_tooltip = "Bright Minecraft things (lava, fire, the portal) glow over GTA's picture, as GTA's own lights do."; > = 0.55;
uniform float BloomThreshold < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Glow threshold"; > = 0.62;
// Set by the add-on: the camera shake, applied to the finished picture so GTA and Minecraft shake together
// (x, y: fraction of the screen height; z: roll, radians), and the nether portal's warp (0..1) while passing through.
uniform float3 Shake = float3(0.0, 0.0, 0.0);
uniform float PortalWarp = 0.0;
uniform float Timer < source = "timer"; >;
uniform int DebugView < ui_type = "combo"; ui_items = "Composite\0GTA depth (1 m bands)\0Minecraft depth (1 m bands)\0Depth difference\0"; > = 0;
uniform bool Reproject < ui_label = "Re-project to GTA's camera"; ui_tooltip = "Rotate Minecraft's (slightly older) frame onto GTA's current camera."; > = true;
uniform float PosePrediction < ui_type = "drag"; ui_min = -2.0; ui_max = 3.0; ui_step = 0.05; ui_label = "Pose prediction (frames)";
	ui_tooltip = "Extrapolate GTA's camera rotation by this many frames before re-projecting."; > = 0.0;

// Set by the add-on: rows of R_mc^T R_gta (GTA camera ray -> Minecraft camera ray), and
// x = tan(GTA vertical fov / 2), y = tan(Minecraft vertical fov / 2), z = Minecraft aspect.
uniform float3 WarpRow0 = float3(1.0, 0.0, 0.0);
uniform float3 WarpRow1 = float3(0.0, 1.0, 0.0);
uniform float3 WarpRow2 = float3(0.0, 0.0, 1.0);
uniform float3 WarpTan = float3(0.7, 0.7, 1.7777);
// Set by the add-on: GTA's camera position relative to Minecraft's, in Minecraft's camera space.
uniform float3 WarpT = float3(0.0, 0.0, 0.0);

// GTA's picture at quarter size with mips: its blurred levels stand in for the light around each pixel.
texture GtaLightTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA8; MipLevels = 7; };
sampler sGtaLight { Texture = GtaLightTex; AddressU = CLAMP; AddressV = CLAMP; };

// The composited world (GTA + Minecraft, no overlay) and, per pixel: Minecraft's coverage, its depth and GTA's.
texture CompositeTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler sComposite { Texture = CompositeTex; AddressU = CLAMP; AddressV = CLAMP; };
texture McInfoTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler sMcInfo { Texture = McInfoTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
// Minecraft's bright parts at quarter size with mips (the glow).
texture McBrightTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; MipLevels = 6; };
sampler sMcBright { Texture = McBrightTex; AddressU = CLAMP; AddressV = CLAMP; };

float luma(float3 c)
{
	return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float4 PS_Light(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(tex2D(ReShade::BackBuffer, uv).rgb, 1.0);
}

/// Minecraft renders at full daylight, so treat its colour as albedo and light it with GTA's local light; then give it
/// GTA's colour grade (the whole picture's colour) and, far away, GTA's haze (fade into the picture around it).
float3 relight(float3 c, float2 uv, float zm, float match, float grade_match)
{
	const float3 L = tex2Dlod(sGtaLight, float4(uv, 0, LightBlur)).rgb;
	const float lum = luma(L);
	const float gain = clamp(lum / LightReference, 0.42, 1.3);
	const float3 tint = clamp(lerp(1.0, L / max(lum, 1e-3), LightTint), 0.55, 1.5);
	c = lerp(c, c * gain * tint, match);
	// the picture's grade, gentled (GTA's own grade keeps some colour in everything) and kept off lights (bright
	// things keep their colour: fire, lava, the portal)
	const float3 M = tex2Dlod(sGtaLight, float4(0.5, 0.5, 0, 6.0)).rgb;
	const float3 grade = clamp(lerp(1.0, M / max(luma(M), 1e-3), 0.6), 0.6, 1.6);
	c = lerp(c, c * grade, grade_match * (1.0 - saturate((max(c.r, max(c.g, c.b)) - 0.45) * 2.5)));
	const float h = saturate((1.0 - exp(-max(zm - HazeStart, 0.0) / HazeDistance)) * HazeStrength);
	const float3 H = tex2Dlod(sGtaLight, float4(uv, 0, LightBlur + 1.0)).rgb;
	return lerp(c, H, h);
}

float mc_linear(float d)
{
	const float n = McPlanes.x, f = McPlanes.y;
	if (d <= 0.0)
		return 1e9;
	return n * f / (n + d * (f - n));
}

/// A source's depth sample in metres along the view axis (1e9: nothing there), by its flags.
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
			continue; // still in front of whatever the source drew there
		// crossed the source's surface: settle on it (fixed-point on the surface point's DS depth)
		float zg = z;
		float2 ndc = n;
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
	const float n = HostPlanes.x, f = HostPlanes.y;
	float z;
	if (HostReversedZ)
		z = d > 0.0 ? n * f / (n + d * (f - n)) : 1e9;
	else
		z = d < 1.0 ? n * f / (f - d * (f - n)) : 1e9;
	return z * HostDepthScale;
}

float3 bands(float z)
{
	const float t = frac(z);
	return lerp(float3(0.1, 0.1, 0.1), float3(1.0, 0.85, 0.3), step(0.5, t)) * saturate(1.5 - z / 100.0);
}

void PS_Composite(float4 pos : SV_Position, float2 uv : TEXCOORD, out float4 outColor : SV_Target0, out float4 outInfo : SV_Target1)
{
	const float3 host = tex2D(ReShade::BackBuffer, uv).rgb;
	outInfo = 0.0;
	outColor = float4(host, 1.0);
	if (!McActive && !AstraActive)
		return;
	const float2 ouv = float2(uv.x, 1.0 - uv.y); // Minecraft's rows are bottom-up

	const float zh = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv, 0, 0)).x);
	// how far behind GTA's surface Minecraft may still show: more where that surface is seen at a grazing angle
	const float allow = min(DepthBias + SlopeBias * abs(ddy(zh)), max(MaxBias, DepthBias));
	const float zFar = max(min(zh + allow, 400.0), 0.5);

	// Minecraft
	float4 world = 0.0;
	float zm = 1e9;
	if (McActive)
	{
		float2 muv = ouv;
		bool inside = true;
		if (Reproject)
			inside = reproject(uv, zFar, sDepth, McPlanes, WarpRow0, WarpRow1, WarpRow2, WarpT, WarpTan, muv, zm);
		else
			zm = mc_linear(tex2D(sDepth, muv).r);
		world = inside ? tex2D(sWorld, muv) : 0.0;
	}
	// Astra
	float4 astra = 0.0;
	float za = 1e9;
	if (AstraActive)
	{
		float2 auv;
		if (reproject(uv, zFar, sAstraDepth, AstraPlanes, AstraWarpRow0, AstraWarpRow1, AstraWarpRow2, AstraWarpT, AstraWarpTan, auv, za))
			astra = tex2D(sAstraWorld, auv);
	}

	outInfo = float4(0.0, 0.0, zh, 0.0);
	if (DebugView == 1)
	{
		outColor = float4(bands(zh), 1.0);
		return;
	}
	if (DebugView == 2)
	{
		outColor = float4(world.a > 0.0 ? bands(zm) : astra.a > 0.0 ? bands(za) : host * 0.3, 1.0);
		return;
	}
	if (DebugView == 3)
	{
		const float zs = world.a > 0.0 ? zm : za;
		outColor = float4((world.a > 0.0 || astra.a > 0.0) ? float3(saturate((zh - zs) * 0.5 + 0.5), saturate(-(zh - zs) * 0.5 + 0.5), 0.0) : host * 0.3, 1.0);
		return;
	}

	// over DS's picture: the farther of Minecraft and Astra first, then the nearer (premultiplied colours)
	const float visM = zm < zh + allow ? 1.0 : 0.0, visA = za < zh + allow ? 1.0 : 0.0;
	const float coverM = world.a * visM, coverA = astra.a * visA;
	const float3 litM = visM > 0.0 ? relight(world.rgb, uv, zm, LightMatch, GradeMatch) : 0.0;
	const float3 litA = visA > 0.0 ? relight(astra.rgb, uv, za, AstraLightMatch, AstraGradeMatch) : 0.0;
	float3 c = host;
	if (za < zm)
	{
		c = litM + c * (1.0 - coverM);
		c = litA + c * (1.0 - coverA);
	}
	else
	{
		c = litA + c * (1.0 - coverA);
		c = litM + c * (1.0 - coverM);
	}
	outColor = float4(c, 1.0);
	const float cover = max(coverM, coverA);
	const float znear = coverA > 0.0 && (za < zm || coverM <= 0.0) ? za : zm;
	// x: anything of ours covers this pixel (edges, contact shadows), y: its depth, z: DS's, w: Minecraft's (glow)
	outInfo = float4(cover, cover > 0.0 ? znear : 0.0, zh, coverM);
}

/// Minecraft's bright parts (lava, fire, the portal, blazes): what glows.
float4 PS_Bright(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float cover = tex2D(sMcInfo, uv).w; // Minecraft's bright things glow; Astra keeps her own look
	const float3 c = tex2D(sComposite, uv).rgb;
	const float l = luma(c);
	return float4(c * (cover * saturate((l - BloomThreshold) / max(1.0 - BloomThreshold, 1e-3)) / max(l, 1e-3)), 1.0);
}

/// The finished picture: the world (shaken as one: GTA and Minecraft together), Minecraft's edges softened, contact
/// shadows on GTA's surfaces next to Minecraft's, Minecraft's glow, the portal's warp, and Minecraft's hand and HUD on
/// top (neither shaken nor warped).
float3 PS_Final(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	if (!McActive && !AstraActive)
		return tex2D(ReShade::BackBuffer, uv).rgb;
	const float aspect = BUFFER_WIDTH * BUFFER_RCP_HEIGHT;
	const float t = Timer * 0.001;
	// shake: rotate and move the picture a little, zoomed in just enough that no edge shows
	const float amount = length(Shake.xy) + abs(Shake.z) * 0.5;
	const float zoom = 1.0 + amount * 2.2 + PortalWarp * 0.06;
	float2 d = (uv - 0.5) * float2(aspect, 1.0) / zoom;
	// the portal: a swirl and waves, strongest in the middle
	if (PortalWarp > 0.0)
	{
		const float r = length(d);
		const float swirl = PortalWarp * 1.1 * exp(-r * 2.2) * sin(t * 1.7 + 1.0);
		const float cs = cos(swirl), sn = sin(swirl);
		d = float2(d.x * cs - d.y * sn, d.x * sn + d.y * cs);
		d += PortalWarp * 0.014 * float2(sin(d.y * 38.0 + t * 7.0), cos(d.x * 31.0 + t * 6.0));
	}
	const float cr = cos(Shake.z), sr = sin(Shake.z);
	d = float2(d.x * cr - d.y * sr, d.x * sr + d.y * cr) + Shake.xy;
	const float2 wuv = d / float2(aspect, 1.0) + 0.5;

	const float2 px = BUFFER_PIXEL_SIZE;
	float3 color = tex2D(sComposite, wuv).rgb;
	const float4 info = tex2D(sMcInfo, wuv);
	// soften where Minecraft meets GTA (GTA's anti-aliasing never saw Minecraft)
	if (EdgeSoftness > 0.0)
	{
		const float c1 = tex2D(sMcInfo, wuv + float2(px.x, 0)).x, c2 = tex2D(sMcInfo, wuv - float2(px.x, 0)).x;
		const float c3 = tex2D(sMcInfo, wuv + float2(0, px.y)).x, c4 = tex2D(sMcInfo, wuv - float2(0, px.y)).x;
		const float edge = saturate(abs(c1 - info.x) + abs(c2 - info.x) + abs(c3 - info.x) + abs(c4 - info.x));
		if (edge > 0.0)
		{
			const float3 n = tex2D(sComposite, wuv + float2(px.x, 0)).rgb + tex2D(sComposite, wuv - float2(px.x, 0)).rgb
				+ tex2D(sComposite, wuv + float2(0, px.y)).rgb + tex2D(sComposite, wuv - float2(0, px.y)).rgb;
			color = lerp(color, (color * 2.0 + n) / 6.0, edge * EdgeSoftness);
		}
	}
	// contact shadows: GTA surface pixels with Minecraft surfaces close by (in depth), mostly above them
	if (ContactShadow > 0.0 && info.x < 0.5 && info.z < 200.0)
	{
		const float zh = info.z;
		const float r = clamp(ContactRadius / max(zh, 0.5) * BUFFER_HEIGHT / (2.0 * WarpTan.x), 2.0, 48.0);
		float occ = 0.0, weight = 0.0;
		[unroll] for (int i = 0; i < 10; ++i)
		{
			const float a = i * 2.39996 + 0.6; // golden-angle spiral
			const float rr = r * sqrt((i + 0.5) / 10.0);
			const float2 o = float2(cos(a), sin(a)) * rr;
			const float4 sm = tex2Dlod(sMcInfo, float4(wuv + o * px, 0, 0));
			const float w = o.y < 0.0 ? 1.0 : 0.35; // things stand on the ground: their shadow is below them
			weight += w;
			if (sm.x > 0.5 && abs(sm.y - zh) < ContactRadius * 1.5)
				occ += w * (1.0 - rr / (r + 1.0));
		}
		color *= 1.0 - ContactShadow * saturate(occ / max(weight, 1e-3) * 2.2);
	}
	// Minecraft's glow
	if (BloomStrength > 0.0)
	{
		const float3 b = tex2Dlod(sMcBright, float4(wuv, 0, 1.0)).rgb * 0.5 + tex2Dlod(sMcBright, float4(wuv, 0, 2.5)).rgb * 0.8
			+ tex2Dlod(sMcBright, float4(wuv, 0, 4.0)).rgb;
		color += b * BloomStrength;
	}
	// the portal's purple
	if (PortalWarp > 0.0)
	{
		const float r = length((uv - 0.5) * float2(aspect, 1.0));
		const float3 purple = color * float3(0.72, 0.38, 1.25) + float3(0.12, 0.0, 0.22) * (0.6 + 0.4 * sin(t * 3.0 + r * 9.0));
		color = lerp(color, purple, saturate(PortalWarp * (0.55 + r * 0.6)));
	}
	// Minecraft's hand and HUD are screen-space: never shaken
	const float4 overlay = McActive ? tex2D(sOverlay, float2(uv.x, 1.0 - uv.y)) : 0.0;
	return overlay.rgb + saturate(color) * (1.0 - overlay.a);
}

technique MCPassthrough < ui_tooltip = "Minecraft passthrough: enabled automatically while the Minecraft link is up."; >
{
	pass Light
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Light;
		RenderTarget = GtaLightTex;
	}
	pass Composite
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Composite;
		RenderTarget0 = CompositeTex;
		RenderTarget1 = McInfoTex;
	}
	pass Bright
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Bright;
		RenderTarget = McBrightTex;
	}
	pass Final
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Final;
	}
}
