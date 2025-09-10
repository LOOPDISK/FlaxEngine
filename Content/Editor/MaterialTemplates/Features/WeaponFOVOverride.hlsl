@0// Weapon FOV Override: Defines
#define USE_WEAPON_FOV_OVERRIDE 1

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
    // Transform world position to view space first
    float4 viewPosition = mul(float4(worldPosition, 1.0), ViewMatrix);
    
    // Calculate FOV scaling factor
    // Camera FOV = 90°, Weapon FOV = 120° (higher FOV makes weapons appear larger)
    // tan(45°) = 1.0, tan(60°) ≈ 1.732
    float cameraFovHalf = radians(45.0); // 90° / 2
    float weaponFovHalf = radians(70.0);  // 120° / 2
    float fovScale = tan(weaponFovHalf) / tan(cameraFovHalf);
    // fovScale = 1.732 / 1.0 = 1.732 (makes weapons appear larger)
    
    // Apply FOV scaling to X and Y in view space (this makes weapons appear larger)
    viewPosition.xy *= fovScale;
    
    // Now apply the normal projection matrix
    // This maintains the same depth relationships as the main camera
    // Reconstruct projection matrix from ViewInfo
    float4x4 projectionMatrix = (float4x4)0;
    projectionMatrix[0][0] = 1.0f / ViewInfo.x; // Projection[0,0]
    projectionMatrix[1][1] = 1.0f / ViewInfo.y; // Projection[1,1]  
    projectionMatrix[2][2] = ViewInfo.z; // (Far / (Far - Near))
    projectionMatrix[2][3] = 1.0f;
    projectionMatrix[3][2] = ViewInfo.w * ViewFar; // (-Far * Near) / (Far - Near)
    return mul(viewPosition, projectionMatrix);
}

// Apply weapon FOV override with custom parameters
float4 ApplyWeaponFOVOverride(float3 worldPosition, float fov, float aspect, float nearPlane, float farPlane)
{
    // Calculate custom projection matrix
    float4x4 weaponProjectionMatrix = CalculateWeaponProjectionMatrix(fov, aspect, nearPlane, farPlane);
    
    // Transform world position to view space
    float4 viewPosition = mul(float4(worldPosition, 1.0), ViewMatrix);
    
    // Apply custom projection
    return mul(viewPosition, weaponProjectionMatrix);
}

#endif // USE_WEAPON_FOV_OVERRIDE

@5// Weapon FOV Override: Shaders