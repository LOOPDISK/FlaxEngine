// Copyright (c) Wojciech Figat. All rights reserved.

#include "FoliageCluster.h"
#include "FoliageInstance.h"
#include "Foliage.h"

namespace
{
    FORCE_INLINE int32 GetActiveChildrenCount()
    {
        return Foliage::GetUse3DClusters() ? 8 : 4;
    }
}

void FoliageCluster::Init(const BoundingBox& bounds)
{
    Bounds = bounds;
    TotalBounds = bounds;
    MaxCullDistance = 0.0f;

    for (int32 i = 0; i < 8; i++)
        Children[i] = nullptr;

    Instances.Clear();
}

void FoliageCluster::UpdateTotalBoundsAndCullDistance()
{
    if (Children[0])
    {
        ASSERT(Instances.IsEmpty());

        const int32 childCount = GetActiveChildrenCount();
        bool hasBounds = false;
        MaxCullDistance = 0.0f;
        for (int32 childIndex = 0; childIndex < childCount; childIndex++)
        {
            FoliageCluster* child = Children[childIndex];
            if (!child)
                continue;
            child->UpdateTotalBoundsAndCullDistance();
            if (!hasBounds)
            {
                TotalBounds = child->TotalBounds;
                hasBounds = true;
            }
            else
            {
                BoundingBox::Merge(TotalBounds, child->TotalBounds, TotalBounds);
            }
            MaxCullDistance = Math::Max(MaxCullDistance, child->MaxCullDistance);
        }
        if (!hasBounds)
        {
            TotalBounds = Bounds;
            MaxCullDistance = 0.0f;
        }
    }
    else if (Instances.HasItems())
    {
        BoundingBox box;
        BoundingBox::FromSphere(Instances[0]->Bounds, TotalBounds);
        MaxCullDistance = Instances[0]->CullDistance;
        for (int32 i = 1; i < Instances.Count(); i++)
        {
            BoundingBox::FromSphere(Instances[i]->Bounds, box);
            BoundingBox::Merge(TotalBounds, box, TotalBounds);
            MaxCullDistance = Math::Max(MaxCullDistance, Instances[i]->CullDistance);
        }
    }
    else
    {
        TotalBounds = Bounds;
        MaxCullDistance = 0;
    }

    BoundingSphere::FromBox(TotalBounds, TotalBoundsSphere);
}

void FoliageCluster::UpdateCullDistance()
{
    if (Children[0])
    {
        const int32 childCount = GetActiveChildrenCount();
        MaxCullDistance = 0.0f;
        for (int32 childIndex = 0; childIndex < childCount; childIndex++)
        {
            FoliageCluster* child = Children[childIndex];
            if (!child)
                continue;
            child->UpdateCullDistance();
            MaxCullDistance = Math::Max(MaxCullDistance, child->MaxCullDistance);
        }
    }
    else if (Instances.HasItems())
    {
        MaxCullDistance = Instances[0]->CullDistance;
        for (int32 i = 1; i < Instances.Count(); i++)
        {
            MaxCullDistance = Math::Max(MaxCullDistance, Instances[i]->CullDistance);
        }
    }
    else
    {
        MaxCullDistance = 0;
    }
}

bool FoliageCluster::Intersects(Foliage* foliage, const Ray& ray, Real& distance, Vector3& normal, FoliageInstance*& instance)
{
    bool result = false;
    Real minDistance = MAX_Real;
    Vector3 minDistanceNormal = Vector3::Up;
    FoliageInstance* minInstance = nullptr;

    if (Children[0])
    {
        const int32 childCount = GetActiveChildrenCount();
        for (int32 childIndex = 0; childIndex < childCount; childIndex++)
        {
            FoliageCluster* child = Children[childIndex];
            if (!child)
                continue;
            if (child->TotalBounds.Intersects(ray) && child->Intersects(foliage, ray, distance, normal, instance) && minDistance > distance)
            {
                minDistanceNormal = normal;
                minDistance = distance;
                minInstance = instance;
                result = true;
            }
        }
    }
    else
    {
        Mesh* mesh;
        for (int32 i = 0; i < Instances.Count(); i++)
        {
            auto& ii = *Instances[i];
            auto& type = foliage->FoliageTypes[ii.Type];
            const Transform transform = foliage->GetTransform().LocalToWorld(ii.Transform);
            if (type.IsReady() && ii.Bounds.Intersects(ray) && type.Model->Intersects(ray, transform, distance, normal, &mesh) && minDistance > distance)
            {
                minDistanceNormal = normal;
                minDistance = distance;
                minInstance = &ii;
                result = true;
            }
        }
    }

    distance = minDistance;
    normal = minDistanceNormal;
    instance = minInstance;
    return result;
}
