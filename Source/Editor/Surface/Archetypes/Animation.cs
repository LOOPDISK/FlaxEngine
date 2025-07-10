// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using FlaxEditor.Scripting;
using FlaxEditor.Surface.Elements;
using FlaxEngine;
using FlaxEngine.GUI;

/// <summary>
/// The coordinate system axis options.
/// </summary>
public enum Axis
{
    /// <summary>
    /// Positive X axis (1, 0, 0)
    /// </summary>
    X = 0,
    /// <summary>
    /// Positive Y axis (0, 1, 0)
    /// </summary>
    Y = 1,
    /// <summary>
    /// Positive Z axis (0, 0, 1)
    /// </summary>
    Z = 2,
    /// <summary>
    /// Negative X axis (-1, 0, 0)
    /// </summary>
    NegativeX = 3,
    /// <summary>
    /// Negative Y axis (0, -1, 0)
    /// </summary>
    NegativeY = 4,
    /// <summary>
    /// Negative Z axis (0, 0, -1)
    /// </summary>
    NegativeZ = 5,
}

/// <summary>
/// The tracking target mode options.
/// </summary>
public enum TrackingTargetMode
{
    /// <summary>
    /// Track another node in the skeleton
    /// </summary>
    Bone = 0,
    /// <summary>
    /// Track a position in world space
    /// </summary>
    Vector = 1,
}

namespace FlaxEditor.Surface.Archetypes
{
    
    
    /// <summary>
    /// Contains archetypes for nodes from the Animation group.
    /// </summary>
    [HideInEditor]
    public static partial class Animation
    {
        /// <summary>
        /// Customized <see cref="SurfaceNode"/> for the main animation graph node.
        /// </summary>
        /// <seealso cref="FlaxEditor.Surface.SurfaceNode" />
        public class Output : SurfaceNode
        {
            /// <inheritdoc />
            public Output(uint id, VisjectSurfaceContext context, NodeArchetype nodeArch, GroupArchetype groupArch)
            : base(id, context, nodeArch, groupArch)
            {
            }
        }

        /// <summary>
        /// Customized <see cref="SurfaceNode"/> for Blend with Mask node.
        /// </summary>
        public class SkeletonMaskSample : SurfaceNode
        {
            private AssetSelect _assetSelect;
            private Box _assetBox;

            /// <inheritdoc />
            public SkeletonMaskSample(uint id, VisjectSurfaceContext context, NodeArchetype nodeArch, GroupArchetype groupArch)
            : base(id, context, nodeArch, groupArch)
            {
            }

            /// <inheritdoc />
            public override void OnSurfaceLoaded(SurfaceNodeActions action)
            {
                base.OnSurfaceLoaded(action);

                if (Surface != null)
                {
                    _assetSelect = GetChild<AssetSelect>();

                    // 4 is the id of skeleton mask parameter node.
                    if (TryGetBox(4, out var box))
                    {
                        _assetBox = box;
                        _assetSelect.Visible = !_assetBox.HasAnyConnection;
                    }
                }
            }

            /// <inheritdoc />
            public override void ConnectionTick(Box box)
            {
                base.ConnectionTick(box);

                if (_assetBox == null)
                    return;
                if (box.ID != _assetBox.ID)
                    return;
                _assetSelect.Visible = !box.HasAnyConnection;
            }
        }

        /// <summary>
        /// Unified Locked Track node that can target either a bone or a world position.
        /// </summary>
        public class LockedTrackNode : SurfaceNode
        {
            private Box _destinationNodeBox;
            private Box _targetPositionBox;
            private Label _destinationNodeLabel;
            private int _currentMode;

            /// <inheritdoc />
            public LockedTrackNode(uint id, VisjectSurfaceContext context, NodeArchetype nodeArch, GroupArchetype groupArch)
            : base(id, context, nodeArch, groupArch)
            {
            }

            /// <inheritdoc />
            public override void OnSurfaceLoaded(SurfaceNodeActions action)
            {
                base.OnSurfaceLoaded(action);

                if (Surface != null)
                {
                    // Find boxes
                    var zeroSuccess = TryGetBox(0, out _destinationNodeBox);
                    var oneSuccess = TryGetBox(1, out _destinationNodeBox);
                    var twoSuccess = TryGetBox(2, out _targetPositionBox);
                    var threeSuccess = TryGetBox(3, out _destinationNodeBox);
                    Debug.Log($"zeroSuccess success: {zeroSuccess}");
                    Debug.Log($"oneSuccess success: {oneSuccess}");
                    Debug.Log($"twoSuccess success: {twoSuccess}");
                    Debug.Log($"threeSuccess success: {threeSuccess}");

                    // Find the destination node label
                    foreach (var element in Elements)
                    {
                        if (element is Label label && label.Text == "Destination Node:")
                        {
                            _destinationNodeLabel = label;
                            break;
                        }
                    }

                    _currentMode = (int)Values[1];
                    UpdateVisibility();
                }
            }

            /// <inheritdoc />
            public override void OnValuesChanged()
            {
                base.OnValuesChanged();

                int newMode = (int)Values[1];
                if (newMode != _currentMode)
                {
                    // Break connections that will be hidden
                    if (newMode == (int)TrackingTargetMode.Vector && _destinationNodeBox != null && _destinationNodeBox.HasAnyConnection)
                    {
                        _destinationNodeBox.RemoveConnections();
                    }
                    else if (newMode == (int)TrackingTargetMode.Bone && _targetPositionBox != null && _targetPositionBox.HasAnyConnection)
                    {
                        _targetPositionBox.RemoveConnections();
                    }

                    _currentMode = newMode;
                    UpdateVisibility();
                }
            }

            private void UpdateVisibility()
            {
                bool isBoneMode = _currentMode == (int)TrackingTargetMode.Bone;

                // Update visibility directly on the boxes
                if (_targetPositionBox != null)
                    _targetPositionBox.Visible = !isBoneMode;


                //Debug.Log("heya", _destinationNodeBox);
                if (_destinationNodeBox != null)
                    _destinationNodeBox.Parent.Visible = isBoneMode;

                if (_destinationNodeLabel != null)
                    _destinationNodeLabel.Parent.Visible = isBoneMode;

                //// Calculate proper height based on Constants.LayoutOffsetY
                //// (7 rows × 20 units) + top/bottom margin + header/footer 
                //int visibleRows = isBoneMode ? 7 : 6; // One fewer row when in Vector mode
                //Height = (visibleRows * Constants.LayoutOffsetY) +
                //         (Constants.NodeMarginY * 2) +
                //         Constants.NodeHeaderSize +
                //         Constants.NodeFooterSize;
            }
        }

        /// <summary>
        /// Customized <see cref="SurfaceNode"/> for the animation sampling nodes
        /// </summary>
        /// <seealso cref="FlaxEditor.Surface.SurfaceNode" />
        public class Sample : SurfaceNode
        {
            private AssetSelect _assetSelect;
            private Box _assetBox;
            private ProgressBar _playbackPos;

            /// <inheritdoc />
            public Sample(uint id, VisjectSurfaceContext context, NodeArchetype nodeArch, GroupArchetype groupArch)
            : base(id, context, nodeArch, groupArch)
            {
            }

            /// <inheritdoc />
            public override void OnValuesChanged()
            {
                base.OnValuesChanged();

                UpdateTitle();
            }

            /// <inheritdoc />
            public override void OnSurfaceLoaded(SurfaceNodeActions action)
            {
                base.OnSurfaceLoaded(action);

                if (Surface != null)
                {
                    _assetSelect = GetChild<AssetSelect>();
                    if (TryGetBox(8, out var box))
                    {
                        _assetBox = box;
                        _assetSelect.Visible = !_assetBox.HasAnyConnection;
                    }
                    UpdateTitle();
                }
            }

            private void UpdateTitle()
            {
                var asset = Editor.Instance.ContentDatabase.Find((Guid)Values[0]);
                if (_assetBox != null)
                    Title = _assetBox.HasAnyConnection || asset == null ? "Animation" : asset.ShortName;
                else
                    Title = asset?.ShortName ?? "Animation";

                var style = Style.Current;
                Resize(Mathf.Max(230, style.FontLarge.MeasureText(Title).X + 30), 160);
            }

            /// <inheritdoc />
            public override void ConnectionTick(Box box)
            {
                base.ConnectionTick(box);

                if (_assetBox == null)
                    return;
                if (box.ID != _assetBox.ID)
                    return;

                _assetSelect.Visible = !box.HasAnyConnection;
                UpdateTitle();
            }

            /// <inheritdoc />
            public override void Update(float deltaTime)
            {
                // Debug current playback position
                if (((AnimGraphSurface)Surface).TryGetTraceEvent(this, out var traceEvent) && traceEvent.Asset is FlaxEngine.Animation anim)
                {
                    if (_playbackPos == null)
                    {
                        _playbackPos = new ProgressBar
                        {
                            SmoothingScale = 0.0f,
                            Offsets = Margin.Zero,
                            AnchorPreset = AnchorPresets.HorizontalStretchBottom,
                            Parent = this,
                            Height = 12.0f,
                        };
                        _playbackPos.Y -= 16.0f;
                    }
                    _playbackPos.Visible = true;
                    _playbackPos.Maximum = anim.Duration;
                    _playbackPos.Value = traceEvent.Value; // AnimGraph reports position in animation frames, not time
                }
                else if (_playbackPos != null)
                {
                    _playbackPos.Visible = false;
                }

                base.Update(deltaTime);
            }
        }

        /// <summary>
        /// Customized <see cref="SurfaceNode"/> for the animation poses blending.
        /// </summary>
        /// <seealso cref="FlaxEditor.Surface.SurfaceNode" />
        public class BlendPose : SurfaceNode
        {
            private readonly List<InputBox> _blendPoses = new List<InputBox>(MaxBlendPoses);
            private Button _addButton;
            private Button _removeButton;

            /// <summary>
            /// The maximum amount of the blend poses to support.
            /// </summary>
            public const int MaxBlendPoses = 8;

            /// <summary>
            /// The index of the first input blend pose box.
            /// </summary>
            public const int FirstBlendPoseBoxIndex = 3;

            /// <summary>
            /// Gets or sets used blend poses count (visible to the user).
            /// </summary>
            public int BlendPosesCount
            {
                get => (int)Values[2];
                set
                {
                    value = Mathf.Clamp(value, 0, MaxBlendPoses);
                    if (value != BlendPosesCount)
                    {
                        SetValue(2, value);
                    }
                }
            }

            /// <inheritdoc />
            public BlendPose(uint id, VisjectSurfaceContext context, NodeArchetype nodeArch, GroupArchetype groupArch)
            : base(id, context, nodeArch, groupArch)
            {
                // Add buttons for adding/removing blend poses
                _addButton = new Button(70, 94, 20, 20)
                {
                    Text = "+",
                    Parent = this
                };
                _addButton.Clicked += () => BlendPosesCount++;
                _removeButton = new Button(_addButton.Right + 4, _addButton.Y, 20, 20)
                {
                    Text = "-",
                    Parent = this
                };
                _removeButton.Clicked += () => BlendPosesCount--;
            }

            private void UpdateBoxes()
            {
                var posesCount = BlendPosesCount;
                while (_blendPoses.Count > posesCount)
                {
                    var boxIndex = _blendPoses.Count - 1;
                    var box = _blendPoses[boxIndex];
                    _blendPoses.RemoveAt(boxIndex);
                    RemoveElement(box);
                }
                while (_blendPoses.Count < posesCount)
                {
                    var ylevel = 3 + _blendPoses.Count;
                    var arch = new NodeElementArchetype
                    {
                        Type = NodeElementType.Input,
                        Position = new Float2(
                                              FlaxEditor.Surface.Constants.NodeMarginX - FlaxEditor.Surface.Constants.BoxOffsetX,
                                              FlaxEditor.Surface.Constants.NodeMarginY + FlaxEditor.Surface.Constants.NodeHeaderSize + ylevel * FlaxEditor.Surface.Constants.LayoutOffsetY),
                        Text = "Pose " + _blendPoses.Count,
                        Single = true,
                        ValueIndex = -1,
                        BoxID = FirstBlendPoseBoxIndex + _blendPoses.Count,
                        ConnectionsType = new ScriptType(typeof(void))
                    };
                    var box = new InputBox(this, arch);
                    AddElement(box);
                    _blendPoses.Add(box);
                }

                _addButton.Enabled = posesCount < MaxBlendPoses;
                _removeButton.Enabled = posesCount > 0;
            }

            private void UpdateHeight()
            {
                float nodeHeight = 10 + (Mathf.Max(_blendPoses.Count, 1) + 3) * FlaxEditor.Surface.Constants.LayoutOffsetY;
                Height = nodeHeight + FlaxEditor.Surface.Constants.NodeMarginY * 2 + FlaxEditor.Surface.Constants.NodeHeaderSize + FlaxEditor.Surface.Constants.NodeFooterSize;
            }

            /// <inheritdoc />
            public override void OnSurfaceLoaded(SurfaceNodeActions action)
            {
                base.OnSurfaceLoaded(action);

                // Peek deserialized boxes
                _blendPoses.Clear();
                for (int i = 0; i < Elements.Count; i++)
                {
                    if (Elements[i] is InputBox box && box.Archetype.BoxID >= FirstBlendPoseBoxIndex)
                    {
                        _blendPoses.Add(box);
                    }
                }

                UpdateBoxes();
                UpdateHeight();
            }

            /// <inheritdoc />
            public override void OnValuesChanged()
            {
                base.OnValuesChanged();

                // Check if update amount of blend pose inputs
                if (_blendPoses.Count != BlendPosesCount)
                {
                    UpdateBoxes();
                    UpdateHeight();
                }
            }
        }

        /// <summary>
        /// The bone transformation modes.
        /// </summary>
        public enum BoneTransformMode
        {
            /// <summary>
            /// No transformation.
            /// </summary>
            None = 0,

            /// <summary>
            /// Applies the transformation.
            /// </summary>
            Add = 1,

            /// <summary>
            /// Replaces the transformation.
            /// </summary>
            Replace = 2,
        }

        /// <summary>
        /// The animated model root motion mode.
        /// </summary>
        public enum RootMotionMode
        {
            /// <summary>
            /// Don't extract nor apply the root motion.
            /// </summary>
            NoExtraction = 0,

            /// <summary>
            /// Ignore root motion (remove from root node transform).
            /// </summary>
            Ignore = 1,

            /// <summary>
            /// Enable root motion (remove from root node transform and apply to the target).
            /// </summary>
            Enable = 2,
        }

        private sealed class AnimationGraphFunctionNode : Function.FunctionNode
        {
            /// <inheritdoc />
            public AnimationGraphFunctionNode(uint id, VisjectSurfaceContext context, NodeArchetype nodeArch, GroupArchetype groupArch)
            : base(id, context, nodeArch, groupArch)
            {
            }

            /// <inheritdoc />
            protected override Asset LoadSignature(Guid id, out string[] types, out string[] names)
            {
                types = null;
                names = null;
                var function = FlaxEngine.Content.Load<AnimationGraphFunction>(id);
                if (function)
                    function.GetSignature(out types, out names);
                return function;
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
                Create = (id, context, arch, groupArch) => new Output(id, context, arch, groupArch),
                Title = "Animation Output",
                Description = "Main animation graph output node",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoRemove | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste | NodeFlags.NoCloseButton,
                Size = new Float2(200, 100),
                DefaultValues = new object[]
                {
                    (int)RootMotionMode.NoExtraction,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "Animation", true, typeof(void), 0),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY, "Root Motion:"),
                    NodeElementArchetype.Factory.ComboBox(80, Surface.Constants.LayoutOffsetY, 100, 0, typeof(RootMotionMode))
                }
            },
            new NodeArchetype
            {
                TypeID = 2,
                Create = (id, context, arch, groupArch) => new Sample(id, context, arch, groupArch),
                Title = "Animation",
                Description = "Animation sampling",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(230, 220),  // Increased height for the new input
                DefaultValues = new object[]
                {
                    Guid.Empty,
                    1.0f,
                    true,
                    0.0f,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, "", typeof(void), 0),
                    NodeElementArchetype.Factory.Output(1, "Normalized Time", typeof(float), 1),
                    NodeElementArchetype.Factory.Output(2, "Time", typeof(float), 2),
                    NodeElementArchetype.Factory.Output(3, "Length", typeof(float), 3),
                    NodeElementArchetype.Factory.Output(4, "Is Playing", typeof(bool), 4),
                    NodeElementArchetype.Factory.Input(0, "Speed", true, typeof(float), 5, 1),
                    NodeElementArchetype.Factory.Input(1, "Loop", true, typeof(bool), 6, 2),
                    NodeElementArchetype.Factory.Input(2, "Start Position", true, typeof(float), 7, 3),
                    NodeElementArchetype.Factory.Input(3, "Animation Asset", true, typeof(FlaxEngine.Animation), 8),
                    NodeElementArchetype.Factory.Input(4, "Position Override", true, typeof(float), 9),
                    NodeElementArchetype.Factory.Asset(0, Surface.Constants.LayoutOffsetY * 5, 0, typeof(FlaxEngine.Animation)),
                }
            },
            new NodeArchetype
            {
                // [Deprecated on 13.05.2020, expires on 13.05.2021]
                TypeID = 3,
                Title = "[deprecated] Transform Bone (local space)",
                Description = "Transforms the skeleton bone",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste,
                Size = new Float2(270, 130),
                DefaultValues = new object[]
                {
                    0,
                    (int)BoneTransformMode.Add,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                    NodeElementArchetype.Factory.Input(1, "Translation", true, typeof(Vector3), 2),
                    NodeElementArchetype.Factory.Input(2, "Rotation", true, typeof(Quaternion), 3),
                    NodeElementArchetype.Factory.Input(3, "Scale", true, typeof(Float3), 4),
                    NodeElementArchetype.Factory.SkeletonBoneIndexSelect(40, Surface.Constants.LayoutOffsetY * 4, 120, 0),
                    NodeElementArchetype.Factory.ComboBox(40, Surface.Constants.LayoutOffsetY * 5, 120, 1, typeof(BoneTransformMode)),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 4, "Bone:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 5, "Mode:"),
                }
            },
            new NodeArchetype
            {
                // [Deprecated on 13.05.2020, expires on 13.05.2021]
                TypeID = 4,
                Title = "[deprecated] Transform Bone (model space)",
                Description = "Transforms the skeleton bone",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste,
                Size = new Float2(270, 130),
                DefaultValues = new object[]
                {
                    0,
                    (int)BoneTransformMode.Add,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                    NodeElementArchetype.Factory.Input(1, "Translation", true, typeof(Vector3), 2),
                    NodeElementArchetype.Factory.Input(2, "Rotation", true, typeof(Quaternion), 3),
                    NodeElementArchetype.Factory.Input(3, "Scale", true, typeof(Float3), 4),
                    NodeElementArchetype.Factory.SkeletonBoneIndexSelect(40, Surface.Constants.LayoutOffsetY * 4, 120, 0),
                    NodeElementArchetype.Factory.ComboBox(40, Surface.Constants.LayoutOffsetY * 5, 120, 1, typeof(BoneTransformMode)),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 4, "Bone:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 5, "Mode:"),
                }
            },
            new NodeArchetype
            {
                // [Deprecated on 15.05.2020, expires on 15.05.2021]
                TypeID = 5,
                Title = "[deprecated] Local To Model",
                Description = "Transforms the skeleton bones from local into model space",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste,
                Size = new Float2(150, 40),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                }
            },
            new NodeArchetype
            {
                // [Deprecated on 15.05.2020, expires on 15.05.2021]
                TypeID = 6,
                Title = "[deprecated] Model To Local",
                Description = "Transforms the skeleton bones from model into local space",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste,
                Size = new Float2(150, 40),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                }
            },
            new NodeArchetype
            {
                // [Deprecated on 13.05.2020, expires on 13.05.2021]
                TypeID = 7,
                Title = "[deprecated] Copy Bone",
                Description = "Copies the skeleton bone transformation data (in local space)",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste,
                Size = new Float2(260, 140),
                DefaultValues = new object[]
                {
                    0,
                    1,
                    true,
                    true,
                    true,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                    NodeElementArchetype.Factory.SkeletonBoneIndexSelect(100, Surface.Constants.LayoutOffsetY * 1, 120, 0),
                    NodeElementArchetype.Factory.SkeletonBoneIndexSelect(100, Surface.Constants.LayoutOffsetY * 2, 120, 1),
                    NodeElementArchetype.Factory.Bool(100, Surface.Constants.LayoutOffsetY * 3, 2),
                    NodeElementArchetype.Factory.Bool(100, Surface.Constants.LayoutOffsetY * 4, 3),
                    NodeElementArchetype.Factory.Bool(100, Surface.Constants.LayoutOffsetY * 5, 4),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 1, "Source Bone:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 2, "Destination Bone:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 3, "Copy Translation:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 4, "Copy Rotation:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 5, "Copy Scale:"),
                }
            },
            new NodeArchetype
            {
                // [Deprecated on 13.05.2020, expires on 13.05.2021]
                TypeID = 8,
                Title = "[deprecated] Get Bone Transform (model space)",
                Description = "Samples the skeleton bone transformation (in model space)",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste,
                Size = new Float2(250, 40),
                DefaultValues = new object[]
                {
                    0,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 0),
                    NodeElementArchetype.Factory.SkeletonBoneIndexSelect(40, Surface.Constants.LayoutOffsetY * 1, 120, 0),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 1, "Bone:"),
                    NodeElementArchetype.Factory.Output(0, "Transform", typeof(Transform), 1),
                }
            },
            new NodeArchetype
            {
                TypeID = 9,
                Title = "Blend",
                Description = "Blend animation poses",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(170, 80),
                DefaultValues = new object[]
                {
                    0.0f,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, "Pose A", true, typeof(void), 1),
                    NodeElementArchetype.Factory.Input(1, "Pose B", true, typeof(void), 2),
                    NodeElementArchetype.Factory.Input(2, "Alpha", true, typeof(float), 3, 0),
                }
            },
            new NodeArchetype
            {
                TypeID = 10,
                Title = "Blend Additive",
                Description =
                "Blend animation poses (with additive mode)" +
                "\n" +
                "\nNote: " +
                "\nThe order of the nodes is important, because additive animation is applied on top of current frame." +
                "\n" +
                "\nTip for blender users:" +
                "\nInside NLA the the order is bottom (first node in flax) to the top (last node in flax)." +
                "\nYou need to place animations in this order to get correct results.",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(170, 80),
                DefaultValues = new object[]
                {
                    0.0f,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, "Base Pose", true, typeof(void), 1),
                    NodeElementArchetype.Factory.Input(1, "Blend Pose", true, typeof(void), 2),
                    NodeElementArchetype.Factory.Input(2, "Blend Alpha", true, typeof(float), 3, 0),
                }
            },
            new NodeArchetype
            {
                TypeID = 11,
                Title = "Blend with Mask",
                Description = "Blend animation poses using skeleton mask",
                Create = (id, context, arch, groupArch) => new SkeletonMaskSample(id, context, arch, groupArch),
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(180, 140),
                DefaultValues = new object[]
                {
                    0.0f,
                    Guid.Empty,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, "Pose A", true, typeof(void), 1),
                    NodeElementArchetype.Factory.Input(1, "Pose B", true, typeof(void), 2),
                    NodeElementArchetype.Factory.Input(2, "Alpha", true, typeof(float), 3, 0),
                    NodeElementArchetype.Factory.Input(3, "Skeleton Mask Asset", true, typeof(SkeletonMask), 4),
                    NodeElementArchetype.Factory.Asset(0, Surface.Constants.LayoutOffsetY * 4, 1, typeof(SkeletonMask)),
                }
            },
            new NodeArchetype
            {
                TypeID = 12,
                Create = (id, context, arch, groupArch) => new MultiBlend1D(id, context, arch, groupArch),
                Title = "Multi Blend 1D",
                Description = "Animation blending in 1D",
                Flags = NodeFlags.AnimGraph | NodeFlags.VariableValuesSize,
                Size = new Float2(420, 320), // Increased height for the new input
                DefaultValues = new object[]
                {
                    // Node data
                    new Float4(0, 100.0f, 0, 0),
                    1.0f,
                    true,
                    0.0f,

                    // Per blend sample data
                    new Float4(0, 0, 0, 1.0f), Guid.Empty,
                },
                Elements = new[]
                {
                    // Output
                    NodeElementArchetype.Factory.Output(0, "", typeof(void), 0),

                    // Options
                    NodeElementArchetype.Factory.Input(0, "Speed", true, typeof(float), 1, 1),
                    NodeElementArchetype.Factory.Input(1, "Loop", true, typeof(bool), 2, 2),
                    NodeElementArchetype.Factory.Input(2, "Start Position", true, typeof(float), 3, 3),

                    // Position Override
                    NodeElementArchetype.Factory.Input(5, "Position Override", true, typeof(float), 5), // New input
        
                    // Axis X
                    NodeElementArchetype.Factory.Input(3, "X", true, typeof(float), 4),
                    NodeElementArchetype.Factory.Text(30, 4 * Surface.Constants.LayoutOffsetY + 2, "(min:                   max:                   )"),
                    NodeElementArchetype.Factory.Vector_X(60, 4 * Surface.Constants.LayoutOffsetY + 2, 0),
                    NodeElementArchetype.Factory.Vector_Y(145, 4 * Surface.Constants.LayoutOffsetY + 2, 0),
                }
            },
            new NodeArchetype
            {
                TypeID = 13,
                Create = (id, context, arch, groupArch) => new MultiBlend2D(id, context, arch, groupArch),
                Title = "Multi Blend 2D",
                Description = "Animation blending in 2D",
                Flags = NodeFlags.AnimGraph | NodeFlags.VariableValuesSize,
                Size = new Float2(420, 640), // Increased height for the new input
                DefaultValues = new object[]
                {
                    // Node data
                    new Float4(0, 100.0f, 0, 100.0f),
                    1.0f,
                    true,
                    0.0f,

                    // Per blend sample data
                    new Float4(0, 0, 0, 1.0f), Guid.Empty,
                },
                Elements = new[]
                {
                    // Output
                    NodeElementArchetype.Factory.Output(0, "", typeof(void), 0),

                    // Options
                    NodeElementArchetype.Factory.Input(0, "Speed", true, typeof(float), 1, 1),
                    NodeElementArchetype.Factory.Input(1, "Loop", true, typeof(bool), 2, 2),
                    NodeElementArchetype.Factory.Input(2, "Start Position", true, typeof(float), 3, 3),
        
                    // Position Override
                    NodeElementArchetype.Factory.Input(6, "Position Override", true, typeof(float), 6), // New input
        
                    // Axis X
                    NodeElementArchetype.Factory.Input(3, "X", true, typeof(float), 4),
                    NodeElementArchetype.Factory.Text(30, 4 * Surface.Constants.LayoutOffsetY + 2, "(min:                   max:                   )"),
                    NodeElementArchetype.Factory.Vector_X(60, 4 * Surface.Constants.LayoutOffsetY + 2, 0),
                    NodeElementArchetype.Factory.Vector_Y(145, 4 * Surface.Constants.LayoutOffsetY + 2, 0),

                    // Axis Y
                    NodeElementArchetype.Factory.Input(4, "Y", true, typeof(float), 5),
                    NodeElementArchetype.Factory.Text(30, 5 * Surface.Constants.LayoutOffsetY + 2, "(min:                   max:                   )"),
                    NodeElementArchetype.Factory.Vector_Z(60, 5 * Surface.Constants.LayoutOffsetY + 2, 0),
                    NodeElementArchetype.Factory.Vector_W(145, 5 * Surface.Constants.LayoutOffsetY + 2, 0),
                }
            },
            new NodeArchetype
            {
                TypeID = 14,
                Create = (id, context, arch, groupArch) => new BlendPose(id, context, arch, groupArch),
                Title = "Blend Poses",
                Description = "Select animation pose to pass by index (with blending)",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(200, 200),
                DefaultValues = new object[]
                {
                    0,
                    0.2f,
                    8,
                    (int)AlphaBlendMode.HermiteCubic,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, "", typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, "Pose Index", true, typeof(int), 1, 0),
                    NodeElementArchetype.Factory.Input(1, "Blend Duration", true, typeof(float), 2, 1),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 2, "Mode:"),
                    NodeElementArchetype.Factory.ComboBox(40, Surface.Constants.LayoutOffsetY * 2, 100, 3, typeof(AlphaBlendMode)),

                    NodeElementArchetype.Factory.Input(3, "Pose 0", true, typeof(void), 3),
                    NodeElementArchetype.Factory.Input(4, "Pose 1", true, typeof(void), 4),
                    NodeElementArchetype.Factory.Input(5, "Pose 2", true, typeof(void), 5),
                    NodeElementArchetype.Factory.Input(6, "Pose 3", true, typeof(void), 6),
                    NodeElementArchetype.Factory.Input(7, "Pose 4", true, typeof(void), 7),
                    NodeElementArchetype.Factory.Input(8, "Pose 5", true, typeof(void), 8),
                    NodeElementArchetype.Factory.Input(9, "Pose 6", true, typeof(void), 9),
                    NodeElementArchetype.Factory.Input(10, "Pose 7", true, typeof(void), 10),
                }
            },
            new NodeArchetype
            {
                TypeID = 15,
                Title = "Get Root Motion",
                Description = "Gets the computed root motion from the pose",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(180, 60),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, "Translation", typeof(Float3), 0),
                    NodeElementArchetype.Factory.Output(1, "Rotation", typeof(Quaternion), 1),
                    NodeElementArchetype.Factory.Input(0, "Pose", true, typeof(void), 2),
                }
            },
            new NodeArchetype
            {
                TypeID = 16,
                Title = "Set Root Motion",
                Description = "Overrides the root motion of the pose",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(180, 60),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, "", typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, "Pose", true, typeof(void), 1),
                    NodeElementArchetype.Factory.Input(1, "Translation", true, typeof(Vector3), 2),
                    NodeElementArchetype.Factory.Input(2, "Rotation", true, typeof(Quaternion), 3),
                }
            },
            new NodeArchetype
            {
                TypeID = 17,
                Title = "Add Root Motion",
                Description = "Applies the custom root motion transformation the root motion of the pose",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(180, 60),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, "", typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, "Pose", true, typeof(void), 1),
                    NodeElementArchetype.Factory.Input(1, "Translation", true, typeof(Vector3), 2),
                    NodeElementArchetype.Factory.Input(2, "Rotation", true, typeof(Quaternion), 3),
                }
            },
            new NodeArchetype
            {
                TypeID = 18,
                Create = (id, context, arch, groupArch) => new StateMachine(id, context, arch, groupArch),
                Title = "State Machine",
                Description = "The animation states machine output node",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(270, 100),
                DefaultValues = new object[]
                {
                    "Locomotion",
                    Utils.GetEmptyArray<byte>(),
                    3,
                    true,
                    true,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, "", typeof(void), 0)
                }
            },
            new NodeArchetype
            {
                TypeID = 19,
                Create = (id, context, arch, groupArch) => new StateMachineEntry(id, context, arch, groupArch),
                Title = "Entry",
                Description = "The animation states machine entry node",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoRemove | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste | NodeFlags.NoCloseButton,
                Size = new Float2(100, 0),
                DefaultValues = new object[]
                {
                    -1,
                },
            },
            new NodeArchetype
            {
                TypeID = 20,
                Create = (id, context, arch, groupArch) => new StateMachineState(id, context, arch, groupArch),
                Title = "State",
                Description = "The animation states machine state node",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste,
                Size = new Float2(100, 0),
                DefaultValues = new object[]
                {
                    "State",
                    Utils.GetEmptyArray<byte>(),
                    Utils.GetEmptyArray<byte>(),
                },
            },
            new NodeArchetype
            {
                TypeID = 21,
                Title = "State Output",
                Description = "The animation states machine state output node",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoRemove | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste | NodeFlags.NoCloseButton,
                Size = new Float2(120, 30),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "Pose", true, typeof(void), 0)
                }
            },
            new NodeArchetype
            {
                TypeID = 22,
                Title = "Rule Output",
                Description = "The animation states machine transition rule output node",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoRemove | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste | NodeFlags.NoCloseButton,
                Size = new Float2(150, 30),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, "Can Start Transition", true, typeof(bool), 0)
                }
            },
            new NodeArchetype
            {
                TypeID = 23,
                Title = "Transition Source State Anim",
                Description = "The animation state machine transition source state animation data information",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste,
                Size = new Float2(270, 110),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, "Length", typeof(float), 0),
                    NodeElementArchetype.Factory.Output(1, "Time", typeof(float), 1),
                    NodeElementArchetype.Factory.Output(2, "Normalized Time", typeof(float), 2),
                    NodeElementArchetype.Factory.Output(3, "Remaining Time", typeof(float), 3),
                    NodeElementArchetype.Factory.Output(4, "Remaining Normalized Time", typeof(float), 4),
                }
            },
            new NodeArchetype
            {
                TypeID = 24,
                Create = (id, context, arch, groupArch) => new AnimationGraphFunctionNode(id, context, arch, groupArch),
                Title = "Animation Graph Function",
                Description = "Calls animation graph function",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(240, 120),
                DefaultValues = new object[]
                {
                    Guid.Empty,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Asset(0, 0, 0, typeof(AnimationGraphFunction)),
                }
            },
            new NodeArchetype
            {
                TypeID = 25,
                Title = "Transform Node (local space)",
                Description = "Transforms the skeleton node",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(280, 130),
                DefaultValues = new object[]
                {
                    string.Empty,
                    (int)BoneTransformMode.Add,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                    NodeElementArchetype.Factory.Input(1, "Translation", true, typeof(Vector3), 2),
                    NodeElementArchetype.Factory.Input(2, "Rotation", true, typeof(Quaternion), 3),
                    NodeElementArchetype.Factory.Input(3, "Scale", true, typeof(Float3), 4),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(40, Surface.Constants.LayoutOffsetY * 4, 120, 0),
                    NodeElementArchetype.Factory.ComboBox(40, Surface.Constants.LayoutOffsetY * 5, 120, 1, typeof(BoneTransformMode)),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 4, "Node:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 5, "Mode:"),
                }
            },
            new NodeArchetype
            {
                TypeID = 26,
                Title = "Transform Node (model space)",
                Description = "Transforms the skeleton node",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(280, 130),
                DefaultValues = new object[]
                {
                    string.Empty,
                    (int)BoneTransformMode.Add,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                    NodeElementArchetype.Factory.Input(1, "Translation", true, typeof(Vector3), 2),
                    NodeElementArchetype.Factory.Input(2, "Rotation", true, typeof(Quaternion), 3),
                    NodeElementArchetype.Factory.Input(3, "Scale", true, typeof(Float3), 4),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(40, Surface.Constants.LayoutOffsetY * 4, 120, 0),
                    NodeElementArchetype.Factory.ComboBox(40, Surface.Constants.LayoutOffsetY * 5, 120, 1, typeof(BoneTransformMode)),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 4, "Node:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 5, "Mode:"),
                }
            },
            new NodeArchetype
            {
                TypeID = 27,
                Title = "Copy Node",
                Description = "Copies the skeleton node transformation data (in local space)",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(260, 140),
                DefaultValues = new object[]
                {
                    string.Empty,
                    string.Empty,
                    true,
                    true,
                    true,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(100, Surface.Constants.LayoutOffsetY * 1, 120, 0),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(100, Surface.Constants.LayoutOffsetY * 2, 120, 1),
                    NodeElementArchetype.Factory.Bool(100, Surface.Constants.LayoutOffsetY * 3, 2),
                    NodeElementArchetype.Factory.Bool(100, Surface.Constants.LayoutOffsetY * 4, 3),
                    NodeElementArchetype.Factory.Bool(100, Surface.Constants.LayoutOffsetY * 5, 4),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 1, "Source Node:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 2, "Destination Node:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 3, "Copy Translation:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 4, "Copy Rotation:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 5, "Copy Scale:"),
                }
            },
            new NodeArchetype
            {
                TypeID = 28,
                Title = "Get Node Transform (model space)",
                Description = "Samples the skeleton node transformation (in model space)",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(324, 40),
                DefaultValues = new object[]
                {
                    string.Empty,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 0),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(40, Surface.Constants.LayoutOffsetY * 1, 160, 0),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 1, "Node:"),
                    NodeElementArchetype.Factory.Output(0, "Transform", typeof(Transform), 1),
                }
            },
            new NodeArchetype
            {
                TypeID = 29,
                Title = "Aim IK",
                Description = "Rotates a node so it aims at a target.",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(250, 80),
                DefaultValues = new object[]
                {
                    string.Empty,
                    1.0f,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                    NodeElementArchetype.Factory.Input(1, "Target", true, typeof(Vector3), 2),
                    NodeElementArchetype.Factory.Input(2, "Weight", true, typeof(float), 3, 1),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(40, Surface.Constants.LayoutOffsetY * 3, 160, 0),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 3, "Node:"),
                }
            },
            new NodeArchetype
            {
                TypeID = 30,
                Title = "Get Node Transform (local space)",
                Description = "Samples the skeleton node transformation (in local space)",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(316, 40),
                DefaultValues = new object[]
                {
                    string.Empty,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 0),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(40, Surface.Constants.LayoutOffsetY * 1, 120, 0),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 1, "Node:"),
                    NodeElementArchetype.Factory.Output(0, "Transform", typeof(Transform), 1),
                }
            },
            new NodeArchetype
            {
                TypeID = 31,
                Title = "Two Bone IK",
                Description = "Performs inverse kinematic on a three nodes chain.",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(250, 140),
                DefaultValues = new object[]
                {
                    string.Empty,
                    1.0f,
                    false,
                    1.5f,
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                    NodeElementArchetype.Factory.Input(1, "Target", true, typeof(Vector3), 2),
                    NodeElementArchetype.Factory.Input(2, "Joint Target", true, typeof(Vector3), 3),
                    NodeElementArchetype.Factory.Input(3, "Weight", true, typeof(float), 4, 1),
                    NodeElementArchetype.Factory.Input(4, "Allow Stretching", true, typeof(bool), 5, 2),
                    NodeElementArchetype.Factory.Input(5, "Max Stretch Scale", true, typeof(float), 6, 3),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(40, Surface.Constants.LayoutOffsetY * 6, 120, 0),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 6, "Node:"),
                }
            },
            new NodeArchetype
            {
                TypeID = 32,
                Title = "Animation Slot",
                Description = "Plays the animation from code with blending (eg. hit reaction).",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(200, 40),
                DefaultValues = new object[]
                {
                    "Default",
                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY, "Slot:"),
                    NodeElementArchetype.Factory.TextBox(30, Surface.Constants.LayoutOffsetY, 140, TextBox.DefaultHeight, 0, false),
                }
            },
            new NodeArchetype
            {
                TypeID = 33,
                Title = "Animation Instance Data",
                Description = "Caches custom data per-instance and allow sampling it. Can be used to randomize animation play offset to offer randomization for crowds reusing the same graph.",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(240, 20),
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, "Get", typeof(Float4), 0),
                    NodeElementArchetype.Factory.Input(0, "Init", true, typeof(Float4), 1),
                }
            },
            new NodeArchetype
            {
                TypeID = 34,
                Create = (id, context, arch, groupArch) => new StateMachineAny(id, context, arch, groupArch),
                Title = "Any",
                Description = "The generic animation states machine state with source transitions from any other state",
                Flags = NodeFlags.AnimGraph | NodeFlags.NoSpawnViaGUI | NodeFlags.NoSpawnViaPaste,
                Size = new Float2(100, 0),
                DefaultValues = new object[]
                {
                    Utils.GetEmptyArray<byte>(),
                },
            },
            // Add these to the existing NodeArchetype[] Nodes array in FlaxEditor.Surface.Archetypes.Animation

             new NodeArchetype
             {
                 TypeID = 35, // New ID for Advanced Blend Additive
                 Title = "Advanced Blend Additive",
                 Description = "Blend animation poses using model-space additive logic relative to a reference pose.",
                 Flags = NodeFlags.AnimGraph,
                 Size = new Float2(170, 100), // Adjusted size for reference pose input
                 DefaultValues = new object[]
                 {
                     0.0f, // Blend Alpha
                 },
                 Elements = new[]
                 {
                     NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                     NodeElementArchetype.Factory.Input(0, "Base Pose", true, typeof(void), 1),
                     NodeElementArchetype.Factory.Input(1, "Additive Pose", true, typeof(void), 2), // Changed text from "Blend Pose"
                     NodeElementArchetype.Factory.Input(2, "Reference Pose", true, typeof(void), 4), // Box ID 4 for Reference Pose
                     NodeElementArchetype.Factory.Input(3, "Blend Alpha", true, typeof(float), 3, 0),
                 }
             },

             new NodeArchetype
             {
                 TypeID = 36, // New ID for Locked Track
                 Create = (id, context, arch, groupArch) => new LockedTrackNode(id, context, arch, groupArch), // Use the custom C# class
                 Title = "Locked Track",
                 Description = "Constrains a node's rotation to track a target (Bone or Vector) while locking one local axis.",
                 Flags = NodeFlags.AnimGraph,
                 Size = new Float2(280, 180), // Adjusted base height
                 DefaultValues = new object[]
                 {
                     string.Empty,                 // Source Node Name (index 0)
                     (int)TrackingTargetMode.Bone, // Tracking mode (index 1)
                     string.Empty,                 // Destination Node Name (index 2) - Used only in Bone mode
                     1.0f,                         // Weight (index 3)
                     (int)Axis.Z,                  // Track axis (index 4)
                     (int)Axis.Y,                  // Locked axis (index 5)
                     // Note: Target Position (Vector3) doesn't have a default value stored here, it's input only
                 },
                 Elements = new[]
                 {
                     NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0), // Output Pose
                     NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1), // Input Pose

                     // --- Controls ---
                     // Mode selector (ValueIndex 1)
                     NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 1, "Track Mode:"),
                     NodeElementArchetype.Factory.ComboBox(100, Surface.Constants.LayoutOffsetY * 1, 160, 1, typeof(TrackingTargetMode)),

                     // Source node (ValueIndex 0)
                     NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 2, "Source Node:"),
                     NodeElementArchetype.Factory.SkeletonNodeNameSelect(100, Surface.Constants.LayoutOffsetY * 2, 160, 0),

                     // Target Position input (BoxID 2) - For Vector mode
                     NodeElementArchetype.Factory.Input(3, "Target Position", true, typeof(Vector3), 2), // Y-level 3

                     // Destination Node (ValueIndex 2) - For Bone mode
                     NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 4, "Destination Node:"), // Y-level 4
                     NodeElementArchetype.Factory.SkeletonNodeNameSelect(100, Surface.Constants.LayoutOffsetY * 4, 160, 2),

                     // Weight input (BoxID 4, ValueIndex 3)
                     NodeElementArchetype.Factory.Input(5, "Weight", true, typeof(float), 4, 3), // Y-level 5

                     // Track Axis (ValueIndex 4)
                     NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 6, "Track Axis:"), // Y-level 6
                     NodeElementArchetype.Factory.ComboBox(100, Surface.Constants.LayoutOffsetY * 6, 160, 4, typeof(Axis)),

                     // Locked Axis (ValueIndex 5)
                     NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 7, "Locked Axis:"), // Y-level 7
                     NodeElementArchetype.Factory.ComboBox(100, Surface.Constants.LayoutOffsetY * 7, 160, 5, typeof(Axis)),
                 }
             },

            new NodeArchetype
            {
                TypeID = 37,
                Title = "track to bone Node",
                Description = "Copies the skeleton node transformation data (in local space)",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(260, 140),
                DefaultValues = new object[]
                {
                    string.Empty,
                    string.Empty,
                    1.0,

                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(100, Surface.Constants.LayoutOffsetY * 1, 120, 0),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(100, Surface.Constants.LayoutOffsetY * 2, 120, 1),
                    //NodeElementArchetype.Factory.Input(3, "up", true, typeof(Vector3), 2),
                    NodeElementArchetype.Factory.Input(4, "weight", true, typeof(float), 2),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 1, "Source Node:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 2, "Destination Node:"),
                }
            },

             new NodeArchetype
            {
                TypeID = 38,
                Title = "locked track Node",
                Description = "Copies the skeleton node transformation data (in local space)",
                Flags = NodeFlags.AnimGraph,
                Size = new Float2(260, 140),
                DefaultValues = new object[]
                {
                    string.Empty,
                    string.Empty,
                    1.0,

                },
                Elements = new[]
                {
                    NodeElementArchetype.Factory.Output(0, string.Empty, typeof(void), 0),
                    NodeElementArchetype.Factory.Input(0, string.Empty, true, typeof(void), 1),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(100, Surface.Constants.LayoutOffsetY * 1, 120, 0),
                    NodeElementArchetype.Factory.SkeletonNodeNameSelect(100, Surface.Constants.LayoutOffsetY * 2, 120, 1),
                    NodeElementArchetype.Factory.Input(3, "weight", true, typeof(float), 2),
                    NodeElementArchetype.Factory.Input(4, "up", true, typeof(Vector3), 3),
                    NodeElementArchetype.Factory.Input(5, "forward", true, typeof(Vector3), 4),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 1, "Source Node:"),
                    NodeElementArchetype.Factory.Text(0, Surface.Constants.LayoutOffsetY * 2, "Destination Node:"),
                }
            },


        };
    }
}
