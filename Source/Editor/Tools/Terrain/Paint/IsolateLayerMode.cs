// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

using FlaxEngine;

namespace FlaxEditor.Tools.Terrain.Paint
{
    /// <summary>
    /// Paint tool mode. Edits terrain splatmap by painting with the single layer on top of the others.
    /// </summary>
    /// <seealso cref="FlaxEditor.Tools.Terrain.Paint.Mode" />
    [HideInEditor]
    public sealed class IsolateLayerMode : Mode
    {
        /// <summary>
        /// The layer to paint directional data into.
        /// </summary>
        [EditorOrder(10), Tooltip("The layer to paint directional data into")]
        public SingleLayerMode.Layers Layer = SingleLayerMode.Layers.Layer0;

        /// <summary>
        /// The target strength value to paint towards (0-1)
        /// </summary>
        [EditorOrder(20), Limit(0, 1, 0.01f), Tooltip("The target strength to paint towards (0-1)")]
        public float TargetStrength = 0.5f;

        /// <inheritdoc />
        public override int ActiveSplatmapIndex => (int)Layer < 4 ? 0 : 1;

        /// <inheritdoc />
        public override unsafe void Apply(ref ApplyParams p)
        {
            var strength = p.Strength;
            var layer = (int)Layer;
            var brushPosition = p.Gizmo.CursorPosition;
            var c = layer % 4;

            Profiler.BeginEvent("Apply Isolate Paint");
            for (int z = 0; z < p.ModifiedSize.Y; z++)
            {
                var zz = z + p.ModifiedOffset.Y;
                for (int x = 0; x < p.ModifiedSize.X; x++)
                {
                    var xx = x + p.ModifiedOffset.X;
                    var src = (Color)p.SourceData[zz * p.HeightmapSize + xx];
                    var samplePositionLocal = p.PatchPositionLocal + new Vector3(xx * FlaxEngine.Terrain.UnitsPerVertex, 0, zz * FlaxEngine.Terrain.UnitsPerVertex);
                    Vector3.Transform(ref samplePositionLocal, ref p.TerrainWorld, out Vector3 samplePositionWorld);
                    var sample = p.Brush.Sample(ref brushPosition, ref samplePositionWorld);

                    if (sample <= 0.0f)
                    {
                        p.TempBuffer[z * p.ModifiedSize.X + x] = src;
                        continue;
                    }

                    var paintAmount = sample * strength;
                    var srcNew = src;
                    // Interpolate towards target strength instead of adding
                    srcNew[c] = Mathf.Lerp(src[c], TargetStrength, paintAmount);
                    p.TempBuffer[z * p.ModifiedSize.X + x] = srcNew;
                }
            }
            Profiler.EndEvent();
            TerrainTools.ModifySplatMap(p.Terrain, ref p.PatchCoord, p.SplatmapIndex, p.TempBuffer, ref p.ModifiedOffset, ref p.ModifiedSize);
        }
    }
}
