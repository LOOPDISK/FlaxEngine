// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.Actions;
using FlaxEditor.Content;
using FlaxEditor.GUI;
using FlaxEditor.SceneGraph;
using FlaxEngine;
using Object = FlaxEngine.Object;

namespace FlaxEditor.Windows.Assets
{
    public sealed partial class PrefabWindow
    {
        /// <summary>
        /// Implementation of <see cref="IUndoAction"/> used to change the root actor of the prefab.
        /// </summary>
        /// <seealso cref="FlaxEditor.IUndoAction" />
        public class SetRootAction : IUndoAction
        {
            private PrefabWindow _window;
            private readonly Guid _before;
            private readonly Guid _after;

            /// <summary>
            /// Initializes a new instance of the <see cref="SetRootAction"/> class.
            /// </summary>
            /// <param name="window">The window reference.</param>
            /// <param name="before">The root before.</param>
            /// <param name="after">The root after.</param>
            internal SetRootAction(PrefabWindow window, Actor before, Actor after)
            {
                _window = window;
                _before = before.ID;
                _after = after.ID;
            }

            private void Set(Guid oldRootId, Guid newRootId)
            {
                var oldRoot = Object.Find<Actor>(ref oldRootId);
                var newRoot = Object.Find<Actor>(ref newRootId);

                _window.Graph.MainActor = null;
                _window.Viewport.Instance = null;

                if (SceneGraphFactory.Nodes.TryGetValue(oldRootId, out var oldRootNode))
                    oldRootNode.Dispose();
                if (SceneGraphFactory.Nodes.TryGetValue(newRootId, out var newRootNode))
                    newRootNode.Dispose();

                newRoot.Parent = null;
                oldRoot.Parent = newRoot;

                _window.Graph.MainActor = newRoot;
                _window.Viewport.Instance = newRoot;
            }

            /// <inheritdoc />
            public string ActionString => "Set root";

            /// <inheritdoc />
            public void Do()
            {
                Set(_before, _after);
            }

            /// <inheritdoc />
            public void Undo()
            {
                Set(_after, _before);
            }

            /// <inheritdoc />
            public void Dispose()
            {
                _window = null;
            }
        }

        /// <summary>
        /// Changes the root object of the prefab.
        /// </summary>
        private void SetRoot()
        {
            var oldRoot = Graph.MainActor;
            var newRoot = ((ActorNode)Selection[0]).Actor;

            Deselect();

            var action = new SetRootAction(this, oldRoot, newRoot);
            action.Do();
            Undo.AddAction(action);
        }

        /// <summary>
        /// Cuts selected objects.
        /// </summary>
        public void Cut()
        {
            Copy();
            Delete();
        }

        /// <summary>
        /// Copies selected objects to system clipboard.
        /// </summary>
        public void Copy()
        {
            // Peek things that can be copied (copy all actors)
            var objects = Selection.Where(x => x.CanCopyPaste).ToList().BuildAllNodes().Where(x => x.CanCopyPaste && x is ActorNode).ToList();
            if (objects.Count == 0)
                return;

            // Serialize actors
            var actors = objects.ConvertAll(x => ((ActorNode)x).Actor);
            var data = Actor.ToBytes(actors.ToArray());
            if (data == null)
            {
                Editor.LogError("Failed to copy actors data.");
                return;
            }

            // Copy data
            Clipboard.RawData = data;
        }

        /// <summary>
        /// Pastes objects from the system clipboard.
        /// </summary>
        public void Paste()
        {
            Paste(null);
        }

        /// <summary>
        /// Pastes the copied objects. Supports undo/redo.
        /// </summary>
        /// <param name="pasteTargetActor">The target actor to paste copied data.</param>
        public void Paste(Actor pasteTargetActor)
        {
            // Get clipboard data
            var data = Clipboard.RawData;

            // Set paste target if only one actor is selected and no target provided
            if (pasteTargetActor == null && Selection.Count == 1 && Selection[0] is ActorNode actorNode)
            {
                pasteTargetActor = actorNode.Actor.IsPrefabRoot ? actorNode.Actor : actorNode.Actor.Parent;
            }

            // Create paste action
            var pasteAction = CustomPasteActorsAction.CustomPaste(this, data, pasteTargetActor?.ID ?? Guid.Empty);
            if (pasteAction != null)
            {
                OnPasteAction(pasteAction);
            }

            // Scroll to new selected node
            ScrollToSelectedNode();
        }

        /// <summary>
        /// Duplicates selected objects.
        /// </summary>
        public void Duplicate()
        {
            // Peek things that can be copied (copy all actors)
            var objects = Selection.Where(x => x.CanDuplicate && x != Graph.Main).ToList().BuildAllNodes().Where(x => x.CanDuplicate && x is ActorNode).ToList();
            if (objects.Count == 0)
                return;

            // Serialize actors
            var actors = objects.ConvertAll(x => ((ActorNode)x).Actor);
            var data = Actor.ToBytes(actors.ToArray());
            if (data == null)
            {
                Editor.LogError("Failed to copy actors data.");
                return;
            }

            // Create paste action (with selecting spawned objects)
            var pasteAction = CustomPasteActorsAction.CustomDuplicate(this, data, Guid.Empty);
            if (pasteAction != null)
            {
                OnPasteAction(pasteAction);
            }

            // Scroll to new selected node
            ScrollToSelectedNode();
        }

        private void OnPasteAction(PasteActorsAction pasteAction)
        {
            pasteAction.Do(out _, out var nodeParents);

            // Select spawned objects
            var selectAction = new SelectionChangeAction(Selection.ToArray(), nodeParents.Cast<SceneGraphNode>().ToArray(), OnSelectionUndo);
            selectAction.Do();

            Undo.AddAction(new MultiUndoAction(pasteAction, selectAction));
            OnSelectionChanges();
        }

        private class CustomDeleteActorsAction : DeleteActorsAction
        {
            public CustomDeleteActorsAction(List<SceneGraphNode> nodes, bool isInverted = false)
            : base(nodes, isInverted)
            {
            }

            /// <inheritdoc />
            protected override void Delete()
            {
                var nodes = _nodeParents.ToArray();

                // Unlink nodes from parents (actors spawned for prefab editing are not in a gameplay and may not send some important events)
                for (int i = 0; i < nodes.Length; i++)
                {
                    if (nodes[i] is ActorNode actorNode)
                        actorNode.Actor.Parent = null;
                }

                base.Delete();

                // Remove nodes (actors in prefab are not in a gameplay and some events from the engine may not be send eg. ActorDeleted)
                for (int i = 0; i < nodes.Length; i++)
                {
                    nodes[i].Dispose();
                }
            }

            /// <inheritdoc />
            protected override SceneGraphNode GetNode(Guid id)
            {
                return SceneGraphFactory.GetNode(id);
            }
        }

        private class CustomPasteActorsAction : PasteActorsAction
        {
            private PrefabWindow _window;

            private CustomPasteActorsAction(PrefabWindow window, byte[] data, Guid[] objectIds, ref Guid pasteParent, string name)
            : base(data, objectIds, ref pasteParent, name)
            {
                _window = window;
            }

            internal static CustomPasteActorsAction CustomPaste(PrefabWindow window, byte[] data, Guid pasteParent)
            {
                var objectIds = Actor.TryGetSerializedObjectsIds(data);
                if (objectIds == null)
                    return null;

                return new CustomPasteActorsAction(window, data, objectIds, ref pasteParent, "Paste actors");
            }

            internal static CustomPasteActorsAction CustomDuplicate(PrefabWindow window, byte[] data, Guid pasteParent)
            {
                var objectIds = Actor.TryGetSerializedObjectsIds(data);
                if (objectIds == null)
                    return null;

                return new CustomPasteActorsAction(window, data, objectIds, ref pasteParent, "Duplicate actors");
            }

            /// <inheritdoc />
            protected override void LinkBrokenParentReference(Actor actor)
            {
                // Link to prefab root
                actor.SetParent(_window.Graph.MainActor, false);
            }

            /// <inheritdoc />
            public override void Undo()
            {
                var nodes = _nodeParents.ToArray();

                for (int i = 0; i < nodes.Length; i++)
                {
                    var node = SceneGraphFactory.FindNode(_nodeParents[i]);
                    if (node != null)
                    {
                        // Unlink nodes from parents (actors spawned for prefab editing are not in a gameplay and may not send some important events)
                        if (node is ActorNode actorNode)
                            actorNode.Actor.Parent = null;

                        // Remove objects
                        node.Delete();

                        // Remove nodes (actors in prefab are not in a gameplay and some events from the engine may not be send eg. ActorDeleted)
                        node.Dispose();
                    }
                }

                _nodeParents.Clear();
            }

            /// <inheritdoc />
            protected override SceneGraphNode GetNode(Guid id)
            {
                return SceneGraphFactory.GetNode(id);
            }

            /// <inheritdoc />
            public override void Dispose()
            {
                base.Dispose();

                _window = null;
            }
        }

        /// <summary>
        /// Resets a nested prefab instance to its default state by deleting it and spawning a fresh copy from the prefab asset.
        /// </summary>
        public void ResetNestedPrefab()
        {
            if (Selection.Count != 1 || !(Selection[0] is ActorNode actorNode))
                return;

            var actor = actorNode.Actor;
            if (!actor.HasPrefabLink)
                return;

            // Check this actor is part of a nested prefab
            var prefabObjectId = actor.PrefabObjectID;
            if (!Asset.GetNestedObject(ref prefabObjectId, out var nestedPrefabId, out _) || nestedPrefabId == Guid.Empty)
                return;

            // Walk up to find the root of the nested prefab instance:
            // the highest ancestor still part of the same nested prefab
            var prefabRoot = actor;
            var current = actor.Parent;
            while (current != null && current != Graph.MainActor)
            {
                var parentObjectId = current.PrefabObjectID;
                if (Asset.GetNestedObject(ref parentObjectId, out var parentNestedId, out _) && parentNestedId == nestedPrefabId)
                {
                    prefabRoot = current;
                }
                else
                {
                    break;
                }
                current = current.Parent;
            }

            var parent = prefabRoot.Parent;
            var localTransform = prefabRoot.LocalTransform;
            var name = prefabRoot.Name;
            var orderInParent = prefabRoot.OrderInParent;

            // Deselect
            var deselectAction = new SelectionChangeAction(Selection.ToArray(), new SceneGraphNode[0], OnSelectionUndo);
            deselectAction.Do();

            // Delete the old instance (prefab window actors need manual scene graph cleanup)
            var node = SceneGraphFactory.FindNode(prefabRoot.ID);
            if (node is ActorNode an)
                an.Actor.Parent = null;
            node?.Delete();
            node?.Dispose();
            FlaxEngine.Scripting.FlushRemovedObjects();

            // Spawn fresh from the nested prefab
            var prefab = FlaxEngine.Content.LoadAsync<Prefab>(nestedPrefabId);
            var newActor = PrefabManager.SpawnPrefab(prefab, parent);
            if (newActor != null)
            {
                newActor.LocalTransform = localTransform;
                newActor.Name = name;
                newActor.OrderInParent = orderInParent;

                // Ensure scene graph node exists (LocalSceneGraph.OnActorSpawned should handle this, but be safe)
                var newNode = SceneGraphFactory.FindNode(newActor.ID) as ActorNode ?? SceneGraphFactory.BuildActorNode(newActor);
                if (newNode != null)
                {
                    var parentNode = SceneGraphFactory.FindNode(parent.ID) as ActorNode;
                    if (parentNode != null)
                        newNode.ParentNode = parentNode;
                    newNode.TreeNode.UnlockChildrenRecursive();
                    Select(newNode);
                }
            }

            MarkAsEdited();
            _treePanel.PerformLayout();
        }

        /// <summary>
        /// Shows a prefab picker, then deletes the selected actor and spawns a fresh instance from the chosen prefab.
        /// Used when a nested prefab instance has lost its prefab link and can't be auto-detected.
        /// </summary>
        public void RelinkToPrefab()
        {
            if (Selection.Count != 1 || !(Selection[0] is ActorNode actorNode))
                return;

            var actor = actorNode.Actor;
            if (actor == Graph.MainActor)
                return;

            // Show prefab asset picker
            AssetSearchPopup.Show(_treePanel, new Float2(_treePanel.Width * 0.5f, _treePanel.Height * 0.5f), item => item is PrefabItem, item =>
            {
                // Re-check selection is still valid
                if (Selection.Count != 1 || !(Selection[0] is ActorNode selectedNode))
                    return;
                var targetActor = selectedNode.Actor;
                if (!targetActor || targetActor == Graph.MainActor)
                    return;

                var parent = targetActor.Parent;
                var localTransform = targetActor.LocalTransform;
                var name = targetActor.Name;
                var orderInParent = targetActor.OrderInParent;

                // Deselect
                Deselect();

                // Delete the old instance (prefab window actors need manual scene graph cleanup)
                var node = SceneGraphFactory.FindNode(targetActor.ID);
                if (node is ActorNode an)
                    an.Actor.Parent = null;
                node?.Delete();
                node?.Dispose();
                FlaxEngine.Scripting.FlushRemovedObjects();

                // Spawn fresh from the chosen prefab
                var prefab = FlaxEngine.Content.LoadAsync<Prefab>(item.ID);
                var newActor = PrefabManager.SpawnPrefab(prefab, parent);
                if (newActor != null)
                {
                    newActor.LocalTransform = localTransform;
                    newActor.Name = name;
                    newActor.OrderInParent = orderInParent;

                    var newNode = SceneGraphFactory.FindNode(newActor.ID) as ActorNode ?? SceneGraphFactory.BuildActorNode(newActor);
                    if (newNode != null)
                    {
                        var parentNode = SceneGraphFactory.FindNode(parent.ID) as ActorNode;
                        if (parentNode != null)
                            newNode.ParentNode = parentNode;
                        newNode.TreeNode.UnlockChildrenRecursive();
                        Select(newNode);
                    }
                }

                MarkAsEdited();
                _treePanel.PerformLayout();
            });
        }

        /// <summary>
        /// Deletes selected objects.
        /// </summary>
        public void Delete()
        {
            // Peek things that can be removed
            var objects = Selection.Where(x => x != null && x.CanDelete && x != Graph.Main).ToList().BuildAllNodes().Where(x => x != null && x.CanDelete).ToList();
            if (objects.Count == 0)
                return;

            // Change selection
            var action1 = new SelectionChangeAction(Selection.ToArray(), new SceneGraphNode[0], OnSelectionUndo);

            // Delete objects
            var action2 = new CustomDeleteActorsAction(objects);

            // Merge actions and perform them
            var action = new MultiUndoAction(new IUndoAction[]
            {
                action1,
                action2
            }, action2.ActionString);
            action.Do();
            Undo.AddAction(action);

            _treePanel.PerformLayout();
            _treePanel.PerformLayout();
        }
    }
}
