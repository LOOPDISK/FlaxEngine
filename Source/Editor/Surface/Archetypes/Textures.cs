// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

using System;
using FlaxEditor.Content.Settings;
using FlaxEditor.GUI;
using FlaxEditor.Scripting;
using FlaxEngine;

namespace FlaxEditor.Surface.Archetypes
{
    /// <summary>
    /// Contains archetypes for nodes from the Textures group.
    /// </summary>
    [HideInEditor]
    public static class Textures
    {
        internal enum CommonSamplerType
        {
            LinearClamp = 0,
            PointClamp = 1,
            LinearWrap = 2,
            PointWrap = 3,
            TextureGroup = 4,
            CubicClamp = 6,
        }

        internal class SampleTextureNode : SurfaceNode
        {
            private ComboBox _textureGroupPicker;

            public SampleTextureNode(uint id, VisjectSurfaceContext context, NodeArchetype nodeArch, GroupArchetype groupArch)
            : base(id, context, nodeArch, groupArch)
            {
            }

            public override void OnValuesChanged()
            {
                base.OnValuesChanged();

                UpdateUI();
            }

            public override void OnLoaded(SurfaceNodeActions action)
            {
                base.OnLoaded(action);

                UpdateUI();
            }

            private void UpdateUI()
            {
                if ((int)Values[0] == (int)CommonSamplerType.TextureGroup)
                {
                    if (_textureGroupPicker == null)
                    {
                        _textureGroupPicker = new ComboBox
                        {
                            Location = new Float2(FlaxEditor.Surface.Constants.NodeMarginX + 50, FlaxEditor.Surface.Constants.NodeMarginY + FlaxEditor.Surface.Constants.NodeHeaderSize + FlaxEditor.Surface.Constants.LayoutOffsetY * 5),
                            Width = 100,
                            Parent = this,
                        };
                        _textureGroupPicker.SelectedIndexChanged += OnSelectedTextureGroupChanged;
                        var groups = GameSettings.Load<StreamingSettings>();
                        if (groups?.TextureGroups != null)
                        {
                            for (int i = 0; i < groups.TextureGroups.Length; i++)
                                _textureGroupPicker.AddItem(groups.TextureGroups[i].Name);
                        }
                    }
                    else
                    {
                        _textureGroupPicker.Visible = true;
                    }
                    _textureGroupPicker.SelectedIndexChanged -= OnSelectedTextureGroupChanged;
                    _textureGroupPicker.SelectedIndex = (int)Values[2];
                    _textureGroupPicker.SelectedIndexChanged += OnSelectedTextureGroupChanged;
                }
                else if (_textureGroupPicker != null)
                {
                    _textureGroupPicker.Visible = false;
                }
                ResizeAuto();
            }

            private void OnSelectedTextureGroupChanged(ComboBox comboBox)
            {
                SetValue(2, _textureGroupPicker.SelectedIndex);
            }
        }

        /// <summary>
        /// The nodes for that group.
        /// </summary>
        public static NodeArchetype[] Nodes =
        {
            new NodeArchetype
            {
                TypeID = 1,
                Title = "Texture",
                Create = (id, context, arch, groupArch) => new Constants.ConvertToParameterNode(id, context, arch, groupArch, new ScriptType(typeof(Texture))),
                Description = "Two dimensional texture object",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(140, 120),
                DefaultValues = new object[]
                {
                    Guid.Empty
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "UVs", true, typeof(Float2), 0),
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(FlaxEngine.Object), 6),
                    NodeElementArchetype.Factory.Output(1, "Color", typeof(Float4), 1),
                    NodeElementArchetype.Factory.Output(2, "R", typeof(float), 2),
                    NodeElementArchetype.Factory.Output(3, "G", typeof(float), 3),
                    NodeElementArchetype.Factory.Output(4, "B", typeof(float), 4),
                    NodeElementArchetype.Factory.Output(5, "A", typeof(float), 5),
                    NodeElementArchetype.Factory.Asset(0, 20, 0, typeof(Texture))
                }
            },
            new NodeArchetype
            {
                TypeID = 2,
                Title = "Texcoords",
                AlternativeTitles = new string[] { "UV", "UVs" },
                Description = "Texture coordinates",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(110, 30),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, "UVs", typeof(Float2), 0)
                }
            },
            new NodeArchetype
            {
                TypeID = 3,
                Title = "Cube Texture",
                Create = (id, context, arch, groupArch) => new Constants.ConvertToParameterNode(id, context, arch, groupArch, new ScriptType(typeof(CubeTexture))),
                Description = "Set of 6 textures arranged in a cube",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(140, 120),
                DefaultValues = new object[]
                {
                    Guid.Empty
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "UVs", true, typeof(Float3), 0),
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(FlaxEngine.Object), 6),
                    NodeElementArchetype.Factory.Output(1, "Color", typeof(Float4), 1),
                    NodeElementArchetype.Factory.Output(2, "R", typeof(float), 2),
                    NodeElementArchetype.Factory.Output(3, "G", typeof(float), 3),
                    NodeElementArchetype.Factory.Output(4, "B", typeof(float), 4),
                    NodeElementArchetype.Factory.Output(5, "A", typeof(float), 5),
                    NodeElementArchetype.Factory.Asset(0, 20, 0, typeof(CubeTexture))
                }
            },
            new NodeArchetype
            {
                TypeID = 4,
                Title = "Normal Map",
                Create = (id, context, arch, groupArch) => new Constants.ConvertToParameterNode(id, context, arch, groupArch, new ScriptType(typeof(NormalMap))),
                Description = "Two dimensional texture object sampled as a normal map",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(140, 120),
                DefaultValues = new object[]
                {
                    Guid.Empty
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "UVs", true, typeof(Float2), 0),
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(FlaxEngine.Object), 6),
                    NodeElementArchetype.Factory.Output(1, "Vector", typeof(Float3), 1),
                    NodeElementArchetype.Factory.Output(2, "X", typeof(float), 2),
                    NodeElementArchetype.Factory.Output(3, "Y", typeof(float), 3),
                    NodeElementArchetype.Factory.Output(4, "Z", typeof(float), 4),
                    NodeElementArchetype.Factory.Asset(0, 20, 0, typeof(Texture))
                }
            },
            new NodeArchetype
            {
                TypeID = 5,
                Title = "Parallax Occlusion Mapping",
                Description = "Parallax occlusion mapping",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(260, 120),
                DefaultValues = new object[]
                {
                    0.05f,
                    8.0f,
                    20.0f,
                    0
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "UVs", true, typeof(Float2), 0),
                    NodeElementArchetype.Factory.Input(1, "Scale", true, typeof(float), 1, 0),
                    NodeElementArchetype.Factory.Input(2, "Min Steps", true, typeof(float), 2, 1),
                    NodeElementArchetype.Factory.Input(3, "Max Steps", true, typeof(float), 3, 2),
                    NodeElementArchetype.Factory.Input(4, "Heightmap Texture", true, typeof(FlaxEngine.Object), 4),
                    NodeElementArchetype.Factory.Output(0, "Parallax UVs", typeof(Float2), 5),
                    NodeElementArchetype.Factory.Text(Surface.Constants.BoxSize + 4, 5 * Surface.Constants.LayoutOffsetY, "Channel"),
                    NodeElementArchetype.Factory.ComboBox(70, 5 * Surface.Constants.LayoutOffsetY, 50, 3, new[]
                    {
                        "R",
                        "G",
                        "B",
                        "A",
                    })
                }
            },
            new NodeArchetype
            {
                TypeID = 6,
                Title = "Scene Texture",
                Description = "Graphics pipeline textures lookup node",
                Flags = NodeFlags.MaterialGraph | NodeFlags.ParticleEmitterGraph,
                Size = new Float2(170, 120),
                DefaultValues = new object[]
                {
                    (int)MaterialSceneTextures.SceneColor
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "UVs", true, typeof(Float2), 0),
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(FlaxEngine.Object), 6),
                    NodeElementArchetype.Factory.Output(1, "Color", typeof(Float4), 1),
                    NodeElementArchetype.Factory.Output(2, "R", typeof(float), 2),
                    NodeElementArchetype.Factory.Output(3, "G", typeof(float), 3),
                    NodeElementArchetype.Factory.Output(4, "B", typeof(float), 4),
                    NodeElementArchetype.Factory.Output(5, "A", typeof(float), 5),
                    NodeElementArchetype.Factory.ComboBox(0, Surface.Constants.LayoutOffsetY, 120, 0, typeof(MaterialSceneTextures))
                }
            },
            new NodeArchetype
            {
                TypeID = 7,
                Title = "Scene Color",
                Description = "Scene color texture lookup node",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(140, 120),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "UVs", true, typeof(Float2), 0),
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(FlaxEngine.Object), 6),
                    NodeElementArchetype.Factory.Output(1, "Color", typeof(Float4), 1),
                    NodeElementArchetype.Factory.Output(2, "R", typeof(float), 2),
                    NodeElementArchetype.Factory.Output(3, "G", typeof(float), 3),
                    NodeElementArchetype.Factory.Output(4, "B", typeof(float), 4),
                    NodeElementArchetype.Factory.Output(5, "A", typeof(float), 5)
                }
            },
            new NodeArchetype
            {
                TypeID = 8,
                Title = "Scene Depth",
                Description = "Scene depth buffer texture lookup node",
                Flags = NodeFlags.MaterialGraph | NodeFlags.ParticleEmitterGraph,
                Size = new Float2(140, 60),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "UVs", true, typeof(Float2), 0),
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(FlaxEngine.Object), 6),
                    NodeElementArchetype.Factory.Output(1, "Depth", typeof(float), 1),
                }
            },
            new NodeArchetype
            {
                TypeID = 9,
                Create = (id, context, arch, groupArch) => new SampleTextureNode(id, context, arch, groupArch),
                Title = "Sample Texture",
                Description = "Custom texture sampling",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(160, 110),
                ConnectionsHints = ConnectionsHint.Vector,
                DefaultValues = new object[]
                {
                    0,
                    -1.0f,
                    0,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "Texture", true, typeof(FlaxEngine.Object), 0),
                    NodeElementArchetype.Factory.Input(1, "UVs", true, null, 1),
                    NodeElementArchetype.Factory.Input(2, "Level", true, typeof(float), 2, 1),
                    NodeElementArchetype.Factory.Input(3, "Offset", true, typeof(Float2), 3),
                    NodeElementArchetype.Factory.Output(0, "Color", typeof(Float4), 4),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 4, "Sampler"),
                    NodeElementArchetype.Factory.ComboBox(50, Surface.Constants.LayoutOffsetY * 4, 100, 0, typeof(CommonSamplerType))
                }
            },
            new NodeArchetype
            {
                TypeID = 10,
                Title = "Flipbook",
                Description = "Texture sheet animation",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(160, 110),
                DefaultValues = new object[]
                {
                    0.0f,
                    new Float2(4, 4),
                    false,
                    false
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "UVs", true, typeof(Float2), 0),
                    NodeElementArchetype.Factory.Input(1, "Frame", true, typeof(float), 1, 0),
                    NodeElementArchetype.Factory.Input(2, "Frames X&Y", true, typeof(Float2), 2, 1),
                    NodeElementArchetype.Factory.Input(3, "Invert X", true, typeof(bool), 3, 2),
                    NodeElementArchetype.Factory.Input(4, "Invert Y", true, typeof(bool), 4, 3),
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(Float2), 5),
                }
            },
            new NodeArchetype
            {
                TypeID = 11,
                Title = "Texture",
                Description = "Two dimensional texture object",
                Flags = NodeFlags.ParticleEmitterGraph,
                Size = new Float2(140, 80),
                DefaultValues = new object[]
                {
                    Guid.Empty
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(FlaxEngine.Object), 0),
                    NodeElementArchetype.Factory.Asset(0, 0, 0, typeof(Texture))
                }
            },
            /*new NodeArchetype
            {
                TypeID = 12,
                Title = "Cube Texture",
                Description = "Set of 6 textures arranged in a cube",
                Flags = NodeFlags.ParticleEmitterGraph,
                Size = new Float2(140, 80),
                DefaultValues = new object[]
                {
                    Guid.Empty
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(FlaxEngine.Object), 0),
                    NodeElementArchetype.Factory.Asset(0, 0, 0, typeof(CubeTexture))
                }
            },*/
            new NodeArchetype
            {
                TypeID = 13,
                Title = "Load Texture",
                Description = "Reads data from the texture at the given location (unsigned integer in range [0; size-1] per axis, last component is a mipmap level)",
                Flags = NodeFlags.ParticleEmitterGraph,
                Size = new Float2(160, 60),
                ConnectionsHints = ConnectionsHint.Vector,
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(Float4), 0),
                    NodeElementArchetype.Factory.Input(0, "Texture", true, typeof(FlaxEngine.Object), 1),
                    NodeElementArchetype.Factory.Input(1, "Location", true, null, 2),
                }
            },
            new NodeArchetype
            {
                TypeID = 14,
                Title = "Sample Global SDF",
                Description = "Samples the Global SDF to get the distance to the closest surface (in world-space). Requires models SDF to be generated and checking `Enable Global SDF` in Graphics Settings.",
                Flags = NodeFlags.MaterialGraph | NodeFlags.ParticleEmitterGraph,
                Size = new Float2(200, 20),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, "Distance", typeof(float), 0),
                    NodeElementArchetype.Factory.Input(0, "World Position", true, typeof(Float3), 1),
                }
            },
            new NodeArchetype
            {
                TypeID = 15,
                Title = "Sample Global SDF Gradient",
                Description = "Samples the Global SDF to get the gradient and distance to the closest surface (in world-space). Normalize gradient to get SDF surface normal vector. Requires models SDF to be generated and checking `Enable Global SDF` in Graphics Settings.",
                Flags = NodeFlags.MaterialGraph | NodeFlags.ParticleEmitterGraph,
                Size = new Float2(260, 40),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, "Gradient", typeof(Float3), 0),
                    NodeElementArchetype.Factory.Output(1, "Distance", typeof(float), 2),
                    NodeElementArchetype.Factory.Input(0, "World Position", true, typeof(Float3), 1),
                }
            },
            new NodeArchetype
            {
                TypeID = 16,
                Title = "World Triplanar Texture",
                Description = "Projects a texture using world-space coordinates instead of UVs.",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(280, 60),
                DefaultValues = new object[]
                {
                    1.0f,
                    1.0f

                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "Texture", true, typeof(FlaxEngine.Object), 0),
                    NodeElementArchetype.Factory.Input(1, "Scale", true, typeof(Float4), 1, 0),
                    NodeElementArchetype.Factory.Input(2, "Blend", true, typeof(float), 2, 1),
                    NodeElementArchetype.Factory.Output(0, "Color", typeof(Float4), 3)
                }
            },
            new NodeArchetype
            {
                TypeID = 17,
                Create = (id, context, arch, groupArch) => new SampleTextureNode(id, context, arch, groupArch),
                Title = "Procedural Sample Texture",
                Description = "Samples a texture to create a more natural look with less obvious tiling.",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(240, 110),
                ConnectionsHints = ConnectionsHint.Vector,
                DefaultValues = new object[]
                {
                    2,
                    -1.0f,
                    0,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "Texture", true, typeof(FlaxEngine.Object), 0),
                    NodeElementArchetype.Factory.Input(1, "UVs", true, null, 1),
                    NodeElementArchetype.Factory.Input(2, "Offset", true, typeof(Float2), 3),
                    NodeElementArchetype.Factory.Output(0, "Color", typeof(Float4), 4),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 4, "Sampler"),
                    NodeElementArchetype.Factory.ComboBox(50, Surface.Constants.LayoutOffsetY * 4, 100, 0, typeof(CommonSamplerType))
                }
            },
            new NodeArchetype
            {
                TypeID = 18,
                Title = "Local Triplanar Texture",
                Description = "Projects a texture using local-space coordinates with offset.",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(280, 140),  // Increased size to accommodate new input
                DefaultValues = new object[]
                {
                    1.0f,
                    1.0f,
                    new Float3(0, 0, 0)  // Default offset
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "Texture", true, typeof(FlaxEngine.Object), 0),
                    NodeElementArchetype.Factory.Input(1, "Scale", true, typeof(Float3), 1, 0),
                    NodeElementArchetype.Factory.Input(2, "Blend", true, typeof(float), 2, 1),
                    NodeElementArchetype.Factory.Input(3, "Offset", true, typeof(Float3), 3, 2),  // New input for offset
                    NodeElementArchetype.Factory.Output(0, "Color", typeof(Float3), 5)
                }
            }
            ,
            new NodeArchetype
            {
                TypeID = 19,
                Title = "Object Triplanar Normal Map",
                Description = " Normal Map using local-space coordinates. using triplanar",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(280, 120),  // Increased size to accommodate new inputs
                DefaultValues = new object[]
                {
                    1.0f,
                    1.0f,
                    new Float3(0, 0, 0)  // Default offset
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "Texture", true, typeof(FlaxEngine.Object), 0),
                    NodeElementArchetype.Factory.Input(1, "Scale", true, typeof(Float3), 1, 0),
                    NodeElementArchetype.Factory.Input(2, "Blend", true, typeof(float), 2, 1),
                    NodeElementArchetype.Factory.Input(3, "Offset", true, typeof(Float3), 3, 2),
                    NodeElementArchetype.Factory.Output(0, "Vector", typeof(Float3), 5)
                }
            },
            new NodeArchetype
            {
                TypeID = 20,
                Title = "OLD Mesh Curvature",
                Description = "Calculates surface curvature based on local-space smooth mesh normals. Output: 0.5 = flat, >0.5 = convex, <0.5 = concave",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(200, 60),
                DefaultValues = new object[]
                {
                    1.0f, // Default strength
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "Strength", true, typeof(float), 0),
                    NodeElementArchetype.Factory.Output(0, "Curvature", typeof(float), 1),
                }
            }

            new NodeArchetype
            {
                TypeID = 21,
                Title = "Lightmap UV",
                AlternativeTitles = new string[] { "Lightmap TexCoord" },
                Description = "Lightmap UVs",
                Flags = NodeFlags.MaterialGraph,
                Size = new Float2(110, 30),
                Elements = new []
                {
                    NodeElementArchetype.Factory.Output(0, "UVs", typeof(Float2), 0)
                }
            }
        };
    }
}
