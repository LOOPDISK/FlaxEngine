@0// Weapon FOV Override: Defines
#define USE_WEAPON_FOV_OVERRIDE 0

@1// Weapon FOV Override: Includes
// Inline shader utilities for weapon FOV override

@2// Weapon FOV Override: Constants
// Hardcoded weapon FOV settings for now

@3// Weapon FOV Override: Resources

@4// Weapon FOV Override: Utilities
#if USE_WEAPON_FOV_OVERRIDE

// Calculate custom weapon projection matrix
float4x4 CalculateWeaponProjectionMatrix(float fov, float aspect, float nearPlane, float farPlane)
{
    float f = 1.0f / tan(fov * 0.5f);
    
    float4x4 projMatrix = (float4x4)0;
    projMatrix[0][0] = f / aspect;
    projMatrix[1][1] = f;
    projMatrix[2][2] = farPlane / (farPlane - nearPlane);
    projMatrix[2][3] = 1.0f;
    projMatrix[3][2] = -(farPlane * nearPlane) / (farPlane - nearPlane);
    
    return projMatrix;
}


// Apply weapon FOV override to world position using default settings
float4 ApplyWeaponFOVOverride(float3 worldPosition, float aspect)
{
    // HACK: Detect if we're in a shadow/depth pass by checking if projection is ortho
    // Ortho projection has [2][3] == 0, perspective has [2][3] == 1
    // Also check if ViewProjection[3][3] == 1 (ortho) vs 0 (perspective)
    float4x4 vp = ViewProjectionMatrix;
    bool isOrtho = (abs(vp[2][3]) < 0.01 && abs(vp[3][3] - 1.0) < 0.01);

    if (isOrtho)
    {
        // Shadow/depth pass with ortho projection - don't apply FOV override
        return mul(float4(worldPosition, 1.0), ViewProjectionMatrix);
    }
    // Transform world position to view space first
    float4 viewPosition = mul(float4(worldPosition, 1.0), ViewMatrix);

    // Create a custom projection matrix with a NARROWER FOV for weapons
    // NARROWER FOV = larger projection values = things appear BIGGER on screen
    // Camera FOV varies, Weapon FOV = 54° (narrower makes things appear larger)
    // Original Projection M11 = 1/tan(cameraFOV/2)/aspect
    // Weapon Projection M11 = 1/tan(weaponFOV/2)/aspect
    // Since we want narrower FOV: weaponFOV < cameraFOV, so weapon M11 > camera M11

    float weaponFovHalf = radians(20.0);  // 54° / 2 (narrower than typical 90° camera FOV)
    float weaponProjectionScale = 1.0f / tan(weaponFovHalf);

    // Build custom projection matrix with narrower FOV but same depth behavior
    // ViewInfo: x=1/P[0,0], y=1/P[1,1], z=Far/(Far-Near), w=(-Far*Near)/(Far-Near)
    float4x4 projectionMatrix = (float4x4)0;
    projectionMatrix[0][0] = weaponProjectionScale / aspect; // Narrower FOV in X = bigger on screen
    projectionMatrix[1][1] = weaponProjectionScale; // Narrower FOV in Y = bigger on screen
    projectionMatrix[2][2] = ViewInfo.z; // Same depth: Far/(Far-Near)
    projectionMatrix[2][3] = 1.0f; // W component
    projectionMatrix[3][2] = ViewInfo.w; // Same depth: (-Far*Near)/(Far-Near)
    return mul(viewPosition, projectionMatrix);
}

// Apply weapon FOV override with custom parameters
float4 ApplyWeaponFOVOverride(float3 worldPosition, float fov, float aspect, float nearPlane, float farPlane)
{
    // HACK: Detect if we're in a shadow/depth pass by checking if projection is ortho
    float4x4 vp = ViewProjectionMatrix;
    bool isOrtho = (abs(vp[2][3]) < 0.01 && abs(vp[3][3] - 1.0) < 0.01);

    if (isOrtho)
    {
        // Shadow/depth pass with ortho projection - don't apply FOV override
        return mul(float4(worldPosition, 1.0), ViewProjectionMatrix);
    }
    // Calculate custom projection matrix
    float4x4 weaponProjectionMatrix = CalculateWeaponProjectionMatrix(fov, aspect, nearPlane, farPlane);

    // Transform world position to view space
    float4 viewPosition = mul(float4(worldPosition, 1.0), ViewMatrix);

    // Apply custom projection
    return mul(viewPosition, weaponProjectionMatrix);
}

#endif // USE_WEAPON_FOV_OVERRIDE

@5// Weapon FOV Override: Shaders