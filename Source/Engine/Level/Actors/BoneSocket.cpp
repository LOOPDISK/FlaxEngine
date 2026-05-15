// Copyright (c) Wojciech Figat. All rights reserved.

#include "BoneSocket.h"
#include "Engine/Level/SceneObjectsFactory.h"
#include "Engine/Serialization/Serialization.h"
#include "AnimatedModel.h"

BoneSocket::BoneSocket(const SpawnParams& params)
    : Actor(params)
    , _index(-1)
    , _useScale(false)
{
}

BoneSocket::~BoneSocket()
{
    if (_parent)
        _parent->_sockets.Remove(this);
}

void BoneSocket::SetNode(const StringView& name)
{
    if (_node != name)
    {
        _node = name;
        _index = -1;
        UpdateTransformation();
    }
}

void BoneSocket::SetUseScale(bool value)
{
    if (_useScale != value)
    {
        _useScale = value;
        UpdateTransformation();
    }
}

void BoneSocket::UpdateTransformation()
{
    AnimatedModel* parent = _parent;
    if (parent && parent->SkinnedModel)
    {
        if (_index == -1)
        {
            _index = parent->SkinnedModel->Skeleton.FindNode(_node);
            if (_index == -1)
                return;
            // TODO: maybe track when skinned model gets unloaded to clear cached node _index?
        }

        auto& nodes = parent->GraphInstance.NodesPose;
        Transform t;
        if (nodes.IsValidIndex(_index))
        {
            nodes.Get()[_index].Decompose(t);
        }
        else if (parent->SkinnedModel->Skeleton.Nodes.IsValidIndex(_index))
        {
            t = parent->SkinnedModel->Skeleton.GetNodeTransform(_index);
        }
        else
        {
            LOG(Error, "BoneSocket '{}' on AnimatedModel '{}': cached node index {} is invalid (skeleton has {} nodes, node name '{}' not found). Skeleton may have changed after reimport.",
                GetNamePath(), parent->GetNamePath(), _index, parent->SkinnedModel->Skeleton.Nodes.Count(), _node);
            _index = -1;
            return;
        }
        if (!_useScale)
            t.Scale = _localTransform.Scale;
        SetLocalTransform(t);
    }
}

#if USE_EDITOR

#include "Engine/Debug/DebugDraw.h"

void BoneSocket::OnDebugDrawSelected()
{
    DEBUG_DRAW_WIRE_SPHERE(BoundingSphere(GetPosition(), 5.0f), Color::BlueViolet, 0, true);

    // Base
    Actor::OnDebugDrawSelected();
}

#endif

void BoneSocket::Serialize(SerializeStream& stream, const void* otherObj)
{
    // Base
    Actor::Serialize(stream, otherObj);

    SERIALIZE_GET_OTHER_OBJ(BoneSocket);

    SERIALIZE_MEMBER(Node, _node);
    SERIALIZE_MEMBER(UseScale, _useScale);
}

void BoneSocket::Deserialize(DeserializeStream& stream, ISerializeModifier* modifier)
{
    // Base
    Actor::Deserialize(stream, modifier);

    _index = -1;
    DESERIALIZE_MEMBER(Node, _node);
    DESERIALIZE_MEMBER(UseScale, _useScale);
    if (IsDuringPlay())
        UpdateTransformation();
}

void BoneSocket::OnTransformChanged()
{
    // Base
    Actor::OnTransformChanged();

    _box = BoundingBox(_transform.Translation);
    _sphere = BoundingSphere(_transform.Translation, 0.0f);
}

void BoneSocket::OnOrderInParentChanged()
{
    // Base
    Actor::OnOrderInParentChanged();

    // Rebuild parent socket cache to match Children order
    if (_parent)
    {
        auto& sockets = _parent->_sockets;
        sockets.Clear();
        for (Actor* child : _parent->Children)
        {
            if (BoneSocket* socket = dynamic_cast<BoneSocket*>(child))
                sockets.Add(socket);
        }
    }
}

void BoneSocket::OnParentChanged()
{
    // Base
    Actor::OnParentChanged();

    // Update cached parent pointer
    AnimatedModel* newParent = dynamic_cast<AnimatedModel*>(GetParent());
    if (newParent != _parent)
    {
        if (_parent)
            _parent->_sockets.Remove(this);
        _parent = newParent;
        if (_parent)
            _parent->_sockets.Add(this);
    }

    if (!IsDuringPlay())
        return;

    _index = -1;
    UpdateTransformation();
}
