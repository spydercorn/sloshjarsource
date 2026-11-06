// Filled Jar: dedicated, marker-selected liquid overlay experiment.
// THIS FILE is not in Data/Shaders and does not override Lighting.hlsl.
// The SKSE plugin renders the normal glass first, then issues an extra draw
// using PSGravityPlane with RTV0 RGB blending and read-only depth.
// v13 gets movement-triggered damped-spring tilt from SKSE PS b12; volume conserved.
// The second-pass prototype has been verified in-game by the mod author.
// v12: slow-motion rocked surface; preserve v11 volume conservation.
// No changes to face selection, opacity, geometry profile or renderer.
// Compare directly against the last known working v3; no changes to gravity or rendering state.
// Keeps the proven shader input register order, world-height math and hook.

cbuffer FilledJarParams : register(b0)
{
    row_major float4x4 ModelToClip;
    row_major float4x4 ModelToWorld;
    float3 LocalFillCenter; // e.g. (0, 0, 20) for the original butterfly jar
    float FillOpacity;     // e.g. 0.65
    float3 LiquidColor;    // e.g. (0.7, 0.02, 0.03)
    float Padding;
};

struct VSInput
{
    float3 LocalPosition : POSITION0;
};

struct PSInput
{
    float4 ClipPosition : SV_POSITION;
    float3 WorldPosition : TEXCOORD0;
    float  FillHeightWorld : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    const float4 localPoint = float4(input.LocalPosition, 1.0);
    output.ClipPosition = mul(localPoint, ModelToClip);
    output.WorldPosition = mul(localPoint, ModelToWorld).xyz;
    output.FillHeightWorld = mul(float4(LocalFillCenter, 1.0), ModelToWorld).z;
    return output;
}

float4 PSMain(PSInput input) : SV_Target0
{
    // The boundary is a WORLD-HORIZONTAL plane even when the jar tips.
    // As with our first experiment, this does NOT conserve liquid volume.
    const float isBelowSurface = input.WorldPosition.z < input.FillHeightWorld ? 1.0 : 0.0;
    return float4(LiquidColor, isBelowSurface * saturate(FillOpacity));
}

// Diagnostic replacement for the currently bound Skyrim pixel shader.
// For the FIRST red-color test only. The renderer keeps its ORIGINAL VS,
// pipeline, textures, render targets and depth/blend state.
// This might not display as bright red in Community Shaders deferred passes;
// it only writes to SV_Target0. It is NOT the final liquid shader.
float4 PSDiagnosticRed(float4 clipPos : SV_POSITION) : SV_Target0
{
    return float4(1.0, 0.015, 0.015, 1.0);
}

// -----------------------------------------------------------------------------
// Filled Jar: shader-interpolator REGISTER-LAYOUT diagnostic.
//
// In RenderDoc, the ORIGINAL lighting VS for Jar03 exports:
//   o0 SV_POSITION     float4
//   o1 TEXCOORD0       float2
//   o2 TEXCOORD1       float3
//   o3 TEXCOORD2       float3
//   o4 TEXCOORD3       float3
//   o5 POSITION1       float4
//   o6 POSITION2       float4
//   o7 COLOR0          float4
//   o8 COLOR1          float4
//   o9 TEXCOORD12      float3  <-- local XYZ used for liquid
//
// Our prior substitute shader used only SV_POSITION + TEXCOORD12, so its
// signature packed TEXCOORD12 into v1. Pixel debugging showed v1 contained
// apparent UV coordinates (0.32657, 0.77809, 0.0), not local XYZ.
//
// Preserve the exact input semantic ORDER and sizes and actually READ each
// varying. The tiny contribution to red's GREEN channel prevents the compiler
// from dropping preceding inputs (which would shift the target register).
//
// CHECK IN RENDERDOC: the compiled PS INPUT SIGNATURE MUST show TEXCOORD12
// at register 9, not register 1. If it doesn't, please send a screenshot and
// DO NOT interpret the visual result as a model-position test.
//
// Mode 1: local-Z top-half test (red above local Z=20, clipped below).
//         Should rotate WITH jar; not gravity aligned yet.
// Mode 2: world-horizontal gravity test using World row 2 from PS b13.
//         Register 9 was verified in RenderDoc on the tested Jar03 pass.
// For overlay mode, the original glass is drawn in a separate, earlier pass;
// PSGravityPlane writes only SV_Target0, with other RTVs masked off by C++.
// -----------------------------------------------------------------------------
// ============================================================================
// EASY LIQUID APPEARANCE SETTINGS - edit only these values to try new potions.
// The plugin recompiles this HLSL on Skyrim launch. No Visual Studio build.
// RGB components are 0.0 (none) to 1.0 (maximum); this is a shader blend tint,
// NOT a real volume-rendered liquid. Results vary with lighting and deferred RT.
// ============================================================================
#define FJ_TINT_RED                 0.45
#define FJ_TINT_GREEN               0.030
#define FJ_TINT_BLUE                0.055
#define FJ_BASE_OPACITY             0.26  // 0=none, 1=strong tint (old test: 0.50)
#define FJ_DEEP_OPACITY_BONUS       0.07  // more saturated toward jar's world-bottom
#define FJ_DEPTH_FADE_DISTANCE     16.0  // in Skyrim model units
#define FJ_DEEP_COLOR_MULTIPLIER    0.82  // slightly darker where liquid is deeper

// ---------- EXPERIMENTAL VIRTUAL LIQUID TOP ----------
// Inspired by procedural bottle shaders and Unreal's virtual-plane tricks.
// The "top" is NOT new geometry: we ray-intersect a horizontal plane through
// the existing glass, then test whether the hit lies inside the jar's body.
// Start with these values. After a successful test, try larger opacity.
#define FJ_CAP_ENABLED              1     // 0 = original working ruby walls only
#define FJ_CAP_FRONT_FACES_ONLY     1     // cap remains on camera-facing shell
#define FJ_WALL_FRONT_FACES_ONLY    1     // NEW: 1 = tint only front-facing walls; 0 = restore v7 wall rendering
#define FJ_LAYER_DEBUG_MODE         0     // 0=combined, 1=cap only, 2=wall only (isolate persistent bands)
#define FJ_CAP_DEBUG                0     // 1 = vivid cyan cap, for diagnosing alignment
// CAP CLIPPING SHAPE. Jar03's five circular rings were measured directly
// from FilledJar.nif. Mode 1 crops the horizontal plane to its revolved
// profile. Mode 0 restores v5/v6 fixed-radius cylinder for a direct A/B test.
#define FJ_CAP_SHAPE_MODE           1     // 1=measured Jar03 profile; 0=old cylinder
#define FJ_PROFILE_WALL_INSET       0.0   // calibrated by user; cap is projected on glass surface (not separate mesh)
#define FJ_CAP_INNER_RADIUS        11.55  // fallback only (shape mode 0)
#define FJ_CAP_MIN_LOCAL_Z          2.25  // v9: reach the closed-end ring (measured Z=2.41089); slight tolerance
#define FJ_CAP_MAX_LOCAL_Z         30.2  // unchanged; below the neck/cork
#define FJ_CAP_EDGE_SOFTNESS        0.18  // unchanged; perimeter transition in local units
#define FJ_CAP_BOTTOM_FADE           0.10  // v9: tighter fade at closed end (previously hard-coded 0.55)
#define FJ_CAP_OPACITY              0.68  // separate from wall opacity
#define FJ_CAP_COLOR_RED            0.69
#define FJ_CAP_COLOR_GREEN          0.13
#define FJ_CAP_COLOR_BLUE           0.18
#define FJ_CAP_RIM_BRIGHTNESS       0.00  // v4: disable virtual cap rim brightening for halo isolation

// ---------- CAP-TO-WALL JUNCTION / SEAM TEST ----------
// The projected cap and the outer glass wall use different intersection
// points. The cap has an inset cylinder radius while the wall follows the
// real NIF silhouette. A tiny intentional contact band covers the gap.
#define FJ_CONTACT_ENABLED         0     // v4: disable extra contact band; test if double outline disappears
#define FJ_CONTACT_HALF_WIDTH      1.25  // WORLD units above/below fill line; try 0.6..2.0
#define FJ_CONTACT_OPACITY         0.58  // opacity of the thin contact band; try 0.35..0.8
#define FJ_CONTACT_HIGHLIGHT       0.09  // extra colored highlight at the meniscus
#define FJ_CONTACT_DEBUG           0     // 1 = green contact band, cyan cap for inspection
#define FJ_CAP_MIN_RAY_RATIO       1.0005 // allow cap very near front glass (v2 = 1.003)

// Keep 2 for world-horizontal liquid. 1 is the old local-Z diagnostic.
#define FILLEDJAR_DIAGNOSTIC_MODE 2

// ---------- v10 VOLUME CONSERVATION (experimental / per-pixel) ----------
// 1: solve a world-horizontal plane enclosing the same approximate liquid
// volume as the upright jar filled to local Z=20. 0: revert to v9 instantly.
// Note: the solve currently runs in every glass fragment. The test is fairly
// expensive: if FPS drops, restore 0 and do not test in VR yet.
#define FJ_USE_VOLUME_CONSERVE       1
#define FJ_VOLUME_SAMPLES           24   // numerical slices; 16=fast, 32=precise
#define FJ_VOLUME_BISECT_STEPS      12   // number of binary-search iterations
#define FJ_VOLUME_Z_MIN             2.41089
#define FJ_VOLUME_Z_MAX            31.61826
#define FJ_VOLUME_PI                3.14159265358979323846

// ---------- v13 MOVEMENT-BASED SLOSH ----------
// Per-jar, damped, movement-triggered surface tilt comes from our plugin.
// PS b12 contains (slopeX, slopeY, 0, 0), written ONLY during the tagged
// Jar03 overlay draw, with the previous b12 restored immediately after.
// These slopes are WORLD XY components; Z remains 1.0, so the surface is
// level when motion settles. The same tilted plane drives volume + top + walls.
#define FJ_SLOSH_ENABLED             1  // 0 = static volume-conserving v11 surface
cbuffer FilledJarMovementSlosh : register(b12)
{
    float4 FJSloshTiltXY : packoffset(c0);
};

cbuffer FilledJarWorldFromVanillaVS : register(b13)
{
    // Exactly the existing VS PerGeometry world matrix copied to PS b13 by
    // our working SKSE DLL. The original glass shader exports WorldPosition
    // in camera-relative world space (camera is the zero origin).
    float4 WorldXRow : packoffset(c0);
    float4 WorldYRow : packoffset(c1);
    float4 WorldZRow : packoffset(c2);
};

struct JarLightingPixelInput
{
    float4 ScreenPosition : SV_POSITION;
    float2 TexCoord0 : TEXCOORD0;
    float3 TBN0 : TEXCOORD1;
    float3 TBN1 : TEXCOORD2;
    float3 TBN2 : TEXCOORD3;
    float4 WorldPosition : POSITION1;
    float4 PreviousWorldPosition : POSITION2;
    float4 VertexColor : COLOR0;
    float4 FogParameters : COLOR1;
    float3 ModelPosition : TEXCOORD12;
    bool IsFrontFace : SV_IsFrontFace; // rasterizer-provided; do not substitute a TEXCOORD register
};

// Convert a WORLD-SPACE DISPLACEMENT to model space without needing the
// camera location or a separate inverse matrix. The cross-product vectors
// form the COLUMNS of the inverse matrix. Works for rotations, reflection,
// and nonzero scaling. Degenerate transforms are only approximately handled.
// The actual Jar03 mesh forms five nearly perfect circular vertex rings.
// Heights and OUTER radii below were extracted from FilledJar.nif (BSTriShape
// Jar03) rather than estimated from a screenshot. Linearly interpolate the
// radius along the jar axis to define an approximate INTERIOR volume.
// This is a revolved profile (works for this rotationally symmetric jar);
// for irregular bottles we would need a 2D angular profile or an SDF.
float FJJarOuterRadiusAtZ(float z)
{
    const float z0 = 2.41089, r0 = 11.13518;
    const float z1 = 6.32365, r1 = 12.32024;
    const float z2 = 25.29461, r2 = 12.32024;
    const float z3 = 28.44017, r3 = 10.11806;
    const float z4 = 31.61826, r4 = 9.27066;
    if (z < z1) return lerp(r0, r1, saturate((z - z0) / (z1 - z0)));
    if (z < z2) return r1;
    if (z < z3) return lerp(r2, r3, saturate((z - z2) / (z3 - z2)));
    return lerp(r3, r4, saturate((z - z3) / (z4 - z3)));
}

float FJCapInteriorRadiusAtZ(float z)
{
#if FJ_CAP_SHAPE_MODE == 1
    return max(0.0, FJJarOuterRadiusAtZ(z) - FJ_PROFILE_WALL_INSET);
#else
    return FJ_CAP_INNER_RADIUS;
#endif
}


// -----------------------------------------------------------------------------
// v11 EXPERIMENTAL VOLUME-PRESERVING FREE SURFACE (upright fix)
//
// For an axisymmetric jar, each small Z slice is a circle whose radius is
// already supplied by FJCapInteriorRadiusAtZ(z). A tilted world-horizontal
// plane slices those circles into segments. We numerically integrate their
// areas and bisect the plane height until its enclosed volume matches the
// upright jar's fill volume.
//
// The solve is PER FRAGMENT in this prototype. Later move it to a once-per-jar
// uniform in the SKSE DLL, or replace it with a small precomputed tilt table.
// -----------------------------------------------------------------------------
float FJDiskArea(float r)
{
    return FJ_VOLUME_PI * r * r;
}

// Area of {x <= t} inside a circle radius r centered at the origin.
float FJDiskSegmentArea(float r, float t)
{
    if (t <= -r) return 0.0;
    if (t >=  r) return FJDiskArea(r);

    float u = clamp(t / max(r, 1e-6), -1.0, 1.0);
    float rootTerm = sqrt(max(r * r - t * t, 0.0));
    return t * rootTerm + r * r * (asin(u) + 0.5 * FJ_VOLUME_PI);
}

// Volume beneath the original upright plane (local Z=fillLocalZ).
// Fractional coverage of each Z slice prevents a visibly quantized starting
// amount (and keeps the upright orientation as close as possible to v9).
float FJUprightVolumeToHeight(float fillLocalZ)
{
    float dz = (FJ_VOLUME_Z_MAX - FJ_VOLUME_Z_MIN) / FJ_VOLUME_SAMPLES;
    float volume = 0.0;
    [loop]
    for (int i = 0; i < FJ_VOLUME_SAMPLES; ++i)
    {
        float z = FJ_VOLUME_Z_MIN + ((float)i + 0.5) * dz;
        float sliceFraction = saturate(0.5 + (fillLocalZ - z) / dz);
        float r = FJCapInteriorRadiusAtZ(z);
        volume += FJDiskArea(r) * (sliceFraction * dz);
    }
    return volume;
}

void FJProjectedHeightBounds(float3 nLocal, out float hMin, out float hMax)
{
    // Radius varies linearly between the five measured mesh rings. The
    // projected extreme of each circular slice is n.z*z +/- |n.xy|*r(z).
    // Therefore the extrema over each profile segment are at its endpoints.
    const float zs[5] = {
        2.41089, 6.32365, 25.29461, 28.44017, 31.61826
    };
    float radial = length(nLocal.xy);
    hMin = 1e30;
    hMax = -1e30;
    [unroll]
    for (int i = 0; i < 5; ++i)
    {
        float r = FJCapInteriorRadiusAtZ(zs[i]);
        float centered = nLocal.z * zs[i];
        hMin = min(hMin, centered - radial * r);
        hMax = max(hMax, centered + radial * r);
    }
}

// Volume beneath dot(nLocal, pLocal) <= hPlane.
float FJVolumeBelowPlane(float3 nLocal, float hPlane)
{
    float dz = (FJ_VOLUME_Z_MAX - FJ_VOLUME_Z_MIN) / FJ_VOLUME_SAMPLES;
    float radial = length(nLocal.xy);
    float volume = 0.0;

    [loop]
    for (int i = 0; i < FJ_VOLUME_SAMPLES; ++i)
    {
        float z = FJ_VOLUME_Z_MIN + ((float)i + 0.5) * dz;
        float r = FJCapInteriorRadiusAtZ(z);
        float belowArea;
        if (radial < 1e-5)
        {
            // When the plane is almost perpendicular to the jar's axis,
            // integrate the partially intersected slice rather than jumping
            // from 0 to 100% coverage at each midpoint.
            float fraction = saturate(0.5 +
                (hPlane - nLocal.z * z) / max(abs(nLocal.z) * dz, 1e-6));
            belowArea = FJDiskArea(r) * fraction;
        }
        else
        {
            float t = (hPlane - nLocal.z * z) / radial;
            belowArea = FJDiskSegmentArea(r, t);
        }
        volume += belowArea * dz;
    }
    return volume;
}

float FJSolveVolumeConservingPlane(float3 nLocal, float targetVolume, float uprightFillLocalZ)
{
    float lo, hi;
    FJProjectedHeightBounds(nLocal, lo, hi);

    // Nearly upright jar: preserve the USER-SELECTED fill height, not a
    // hard-coded value from an earlier prototype. Express that local height
    // in the same plane-coordinate system as dot(nLocal, modelPosition).
    // This avoids the expensive numerical solve for normal standing jars.
    if (nLocal.z > 0.99999)
        return nLocal.z * uprightFillLocalZ;

    [loop]
    for (int i = 0; i < FJ_VOLUME_BISECT_STEPS; ++i)
    {
        float mid = 0.5 * (lo + hi);
        float v = FJVolumeBelowPlane(nLocal, mid);
        if (v < targetVolume) lo = mid;
        else                  hi = mid;
    }
    return 0.5 * (lo + hi);
}

float3 WorldDeltaToLocal(float3 deltaWorld)
{
    float3 r0 = WorldXRow.xyz;
    float3 r1 = WorldYRow.xyz;
    float3 r2 = WorldZRow.xyz;
    float3 inv0 = cross(r1, r2);
    float3 inv1 = cross(r2, r0);
    float3 inv2 = cross(r0, r1);
    float determinant = dot(r0, inv0);
    float safeDet = (abs(determinant) > 1e-5) ? determinant : 1e-5;
    // The cross products are the COLUMNS of the inverse matrix.
    // Combine them with the corresponding WORLD components, rather than
    // dotting each cross product against the world displacement (which
    // applies the TRANSPOSED inverse and misplaces the cap on rotated jars).
    return (deltaWorld.x * inv0 +
            deltaWorld.y * inv1 +
            deltaWorld.z * inv2) / safeDet;
}

float4 PSGravityPlane(JarLightingPixelInput input) : SV_Target0
{
    // IMPORTANT: Retain the original Skyrim VS output register layout.
    // RenderDoc confirmed that TEXCOORD12 is output register o9, not o1.
    // Consuming these preceding inputs prevents D3DCompile from packing the
    // local position into an incompatible input register.
    float dummySum =
          input.TexCoord0.x
        + input.TBN0.x
        + input.TBN1.x
        + input.TBN2.x
        + input.WorldPosition.x
        + input.PreviousWorldPosition.x
        + input.VertexColor.x
        + input.FogParameters.x;
    float keepLayoutAlive = frac(abs(dummySum) * 0.137) * 0.0001;

    float3 centerLocal = float3(0.0, 0.0, 5.0);

    // World-space free-surface normal. At rest it is +world Z (gravity).
    // In movement mode, the jar's damped-spring state tilts the free surface.
    // The volume solver still repositions the plane to conserve the liquid.
    float3 freeSurfaceNormalWorld = float3(0.0, 0.0, 1.0);
#if FJ_SLOSH_ENABLED && FILLEDJAR_DIAGNOSTIC_MODE == 2
    // Values evolve only after the jar moves, then decay toward zero.
    freeSurfaceNormalWorld = normalize(float3(FJSloshTiltXY.xy, 1.0));
#endif

    // Convert the WORLD plane normal to the jar's model coordinates.
    // WorldX/Y/ZRow are the rows of the model-to-camera-relative-world
    // transform (the same verified matrix from PS b13).
    float3 localPlaneVector =
          freeSurfaceNormalWorld.x * WorldXRow.xyz
        + freeSurfaceNormalWorld.y * WorldYRow.xyz
        + freeSurfaceNormalWorld.z * WorldZRow.xyz;
    float rowLen = max(length(localPlaneVector), 1e-6);
    float3 planeNormalLocal = localPlaneVector / rowLen;
    float solvedPlaneLocalH = dot(planeNormalLocal, centerLocal);

#if FJ_USE_VOLUME_CONSERVE && FILLEDJAR_DIAGNOSTIC_MODE == 2
    float targetVolume = FJUprightVolumeToHeight(centerLocal.z);
    solvedPlaneLocalH = FJSolveVolumeConservingPlane(
        planeNormalLocal, targetVolume, centerLocal.z);
#endif

    float signedDepth;
#if FILLEDJAR_DIAGNOSTIC_MODE == 1
    signedDepth = input.ModelPosition.z - centerLocal.z;
#else
    // Positive below the same solved world-horizontal plane that the cap uses.
    signedDepth = solvedPlaneLocalH - dot(planeNormalLocal, input.ModelPosition);
#endif

    float wallAlpha = 0.0;
    float3 wallColor = float3(FJ_TINT_RED, FJ_TINT_GREEN, FJ_TINT_BLUE);
    // For the overlay pass, both near and far glass shell triangles can be
    // rasterized. Alpha blending of both sides can create an extra red layer
    // at oblique angles, particularly on sideways jars. Test only the front
    // shell: the projected cap already uses the same front-facing rule.
    // This is intentionally opt-in and reversible; the original glass pass
    // remains completely unchanged.
    bool allowWallTint = (FJ_WALL_FRONT_FACES_ONLY == 0) || input.IsFrontFace;
    if (signedDepth > 0.0 && allowWallTint) {
        float depthAmount = saturate(signedDepth / max(FJ_DEPTH_FADE_DISTANCE, 0.001));
        wallAlpha = saturate(FJ_BASE_OPACITY + FJ_DEEP_OPACITY_BONUS * depthAmount);
        wallColor *= lerp(1.0, FJ_DEEP_COLOR_MULTIPLIER, depthAmount);
    }

    // --------------------------------------------------------------------
    // VIRTUAL LIQUID CAP, rendered THROUGH the original glass mesh.
    //
    // WorldPosition is camera-relative (the eye is at 0,0,0). Project the
    // ray from the camera through this fragment onto the tilted
    // moving fill plane. If that hit is BEHIND this glass fragment and inside the
    // jar's interior, blend a second surface tint at this pixel.
    //
    // This is a first approximation of the inner volume as a cylinder.
    // The test depends on the original lighting VS and our known jar model.
    // No new D3D draw, depth buffer, shader registration, or DLL rebuild.
    // --------------------------------------------------------------------
    float capAlpha = 0.0;
    float3 capColor = float3(FJ_CAP_COLOR_RED, FJ_CAP_COLOR_GREEN, FJ_CAP_COLOR_BLUE);
#if FJ_CAP_ENABLED && FILLEDJAR_DIAGNOSTIC_MODE == 2
    float3 glassWorld = input.WorldPosition.xyz;
    // Plane equation in camera-relative WORLD coordinates:
    // dot(freeSurfaceNormalWorld, worldPoint) = fillPlaneWorld.
    // Use the same plane as the wall tint, INCLUDING its new tilt.
    float3 worldTranslation = float3(WorldXRow.w, WorldYRow.w, WorldZRow.w);
    float fillPlaneWorld = solvedPlaneLocalH * rowLen +
        dot(freeSurfaceNormalWorld, worldTranslation);
    float rayPlaneDot = dot(freeSurfaceNormalWorld, glassWorld);
    // The user sees the liquid's upper surface primarily when the camera
    // is ABOVE it. Skip steeply horizontal view rays and below-surface views.
    // Only the nearest camera-facing glass shell should project the cap.
    // This is an A/B hypothesis: v4 may blend projected surfaces from
    // front and back glass triangles in the second pass. Wall tint remains unchanged.
    bool allowCapProjection = (FJ_CAP_FRONT_FACES_ONLY == 0) || input.IsFrontFace;
    if (allowCapProjection && fillPlaneWorld < -0.01 && rayPlaneDot < -0.001) {
        float rayT = fillPlaneWorld / rayPlaneDot;
        // Must be beyond the currently shaded glass wall and in front of eye.
        if (rayT > FJ_CAP_MIN_RAY_RATIO && rayT < 25.0) {
            float3 capWorld = glassWorld * rayT;
            float3 capLocal = input.ModelPosition +
                WorldDeltaToLocal(capWorld - glassWorld);
            float radius = length(capLocal.xy);
            float interiorRadius = FJCapInteriorRadiusAtZ(capLocal.z);
            float radialCoverage = saturate((interiorRadius - radius) /
                                             max(FJ_CAP_EDGE_SOFTNESS, 0.01));
            // Crop the WORLD-HORIZONTAL virtual plane to the model's
            // revolved interior profile, not a circle of one fixed radius.
            // When the jar tips, z varies across the plane and the boundary
            // becomes a curved intersection with the true jar silhouette.
            float bottomMask = saturate((capLocal.z - FJ_CAP_MIN_LOCAL_Z) /
                                        max(FJ_CAP_BOTTOM_FADE, 0.001));
            float topMask = saturate((FJ_CAP_MAX_LOCAL_Z - capLocal.z) / 0.55);
            float coverage = radialCoverage * bottomMask * topMask;
            if (coverage > 0.0) {
                // Brighter at the meniscus, with a gentle broad highlight.
                float rim = saturate((interiorRadius - radius) / 1.7);
                float meniscus = 1.0 - smoothstep(0.0, 0.5, rim);
                capColor += FJ_CAP_RIM_BRIGHTNESS * meniscus;
                capColor += 0.055 * saturate(-glassWorld.z / max(length(glassWorld), 0.001));
#if FJ_CAP_DEBUG
                capColor = float3(0.025, 0.92, 0.86); // striking cyan top only
#endif
                capAlpha = saturate(FJ_CAP_OPACITY * coverage);
            }
        }
    }
#endif

    // Continuously COMPOSITE the virtual cap over the wall tint, rather than
    // switching colors as soon as capAlpha becomes nonzero.
    // Previous version's hard `if (capAlpha > 0)` made even a 0.1% cap edge
    // suddenly adopt full capColor at the wall's opacity, producing a band.
    // Use premultiplied-alpha math internally; the renderer still receives
    // ordinary straight RGBA for its existing SRC_ALPHA blending state.
    float resultAlpha = capAlpha + wallAlpha * (1.0 - capAlpha);
    float3 resultPremul = capColor * capAlpha +
                          wallColor * wallAlpha * (1.0 - capAlpha);
    float3 resultColor = resultPremul / max(resultAlpha, 0.00001);

    // A/B switches: if banding survives, test the cap and wall separately.
    // The SKSE plugin and the HLSL register layout remain unchanged.
#if FJ_LAYER_DEBUG_MODE == 1
    resultAlpha = capAlpha;
    resultColor = capColor;
#elif FJ_LAYER_DEBUG_MODE == 2
    resultAlpha = wallAlpha;
    resultColor = wallColor;
#endif

#if FJ_CONTACT_ENABLED && FILLEDJAR_DIAGNOSTIC_MODE == 2
    // A thin colored meniscus directly on the glass, *centered* on the
    // world-horizontal fill plane. This visually bridges the gap between
    // the cap's inset cylindrical silhouette and the actual jar wall.
    // This works for arbitrary rotation: signedDepth is WORLD-height.
    float contactDistance = abs(signedDepth);
    float contactWeight = 1.0 - smoothstep(
        0.0, max(FJ_CONTACT_HALF_WIDTH, 0.01), contactDistance);
    float contactAlpha = saturate(FJ_CONTACT_OPACITY * contactWeight);
    float3 contactColor = saturate(capColor +
        FJ_CONTACT_HIGHLIGHT * contactWeight);
#if FJ_CONTACT_DEBUG
    contactColor = float3(0.05, 0.95, 0.1); // green meniscus; cap remains red/cyan
#endif
    if (contactAlpha > resultAlpha) {
        // Lift near-contact pixels above the wall tint so the perimeter
        // does not fall into a transparent/dark annulus.
        resultColor = lerp(resultColor, contactColor, contactWeight);
        resultAlpha = contactAlpha;
    }
#endif

    clip(resultAlpha - 0.0005);
    resultColor.g += keepLayoutAlive;
    return float4(saturate(resultColor), saturate(resultAlpha));
}
