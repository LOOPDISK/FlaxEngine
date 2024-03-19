float3 ApplyPreGammaTransform(float3 color)
{
    // Apply the transformation to each channel
    color.r = color.r * color.r * (3.0 - 2.0 * color.r);
    color.g = color.g * color.g * (3.0 - 2.0 * color.g);
    color.b = color.b * color.b * (3.0 - 2.0 * color.b);

    return color;
}
