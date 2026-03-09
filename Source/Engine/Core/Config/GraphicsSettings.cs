namespace FlaxEditor.Content.Settings
{
    public partial class GraphicsSettings
    {
        /// <summary>
        /// Whether to show the Noise Scale setting (only for Procedural3D distortion mode).
        /// </summary>
        public bool ShowStylizedCloudNoiseScale => StylizedCloudDistortionMode == FlaxEngine.StylizedCloudDistortionMode.Procedural3D;

        /// <summary>
        /// Whether to show the Distortion Cube Map setting (hidden for Procedural3D distortion mode).
        /// </summary>
        public bool ShowStylizedCloudCubeMap => StylizedCloudDistortionMode != FlaxEngine.StylizedCloudDistortionMode.Procedural3D;
    }
}
