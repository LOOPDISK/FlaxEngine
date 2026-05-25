// Copyright (c) Wojciech Figat. All rights reserved.

#include "MaterialBase.h"
#include "MaterialInstance.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/LogContext.h"
#include "Engine/Core/Types/Variant.h"
#include "Engine/Content/Content.h"
#include "Engine/Content/Factories/BinaryAssetFactory.h"

REGISTER_BINARY_ASSET_ABSTRACT(MaterialBase, "FlaxEngine.MaterialBase");

MaterialBase::MaterialBase(const SpawnParams& params, const AssetInfo* info)
    : BinaryAsset(params, info)
{
}

Variant MaterialBase::GetParameterValue(const StringView& name)
{
    if (!IsLoaded() && WaitForLoaded())
        return Variant::Null;
    const auto param = Params.Get(name);
    if (IsMaterialInstance() && param && !param->IsOverride() && ((MaterialInstance*)this)->GetBaseMaterial())
        return ((MaterialInstance*)this)->GetBaseMaterial()->GetParameterValue(name);
    if (param)
        return param->GetValue();
    const String context = LogContext::FormatContext();
    LOG(Warning, "Missing material parameter '{0}' in material {1}.{2}", String(name), ToString(), context);
    LogContext::Print(LogType::Warning);
    return Variant::Null;
}

void MaterialBase::SetParameterValue(const StringView& name, const Variant& value, bool warnIfMissing)
{
    if (!IsLoaded() && WaitForLoaded())
        return;
    const auto param = Params.Get(name);
    if (param)
    {
        param->SetValue(value);
        param->SetIsOverride(true);
    }
    else if (warnIfMissing)
    {
        const String context = LogContext::FormatContext();
        LOG(Warning, "Missing material parameter '{0}' in material {1}.{2}", name, ToString(), context);
        LogContext::Print(LogType::Warning);
    }
}

MaterialInstance* MaterialBase::CreateVirtualInstance()
{
    auto instance = Content::CreateVirtualAsset<MaterialInstance>();
    instance->SetBaseMaterial(this);
    return instance;
}

#if USE_EDITOR

void MaterialBase::GetReferences(Array<Guid>& assets, Array<String>& files) const
{
    BinaryAsset::GetReferences(assets, files);
    Params.GetReferences(assets);
}

#endif
