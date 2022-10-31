// Copyright (c) 2012-2022 Wojciech Figat. All rights reserved.

#include "NetworkReplicator.h"
#include "NetworkClient.h"
#include "NetworkManager.h"
#include "NetworkInternal.h"
#include "NetworkStream.h"
#include "INetworkSerializable.h"
#include "NetworkMessage.h"
#include "NetworkPeer.h"
#include "NetworkChannelType.h"
#include "NetworkEvent.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/Collections/Dictionary.h"
#include "Engine/Platform/CriticalSection.h"
#include "Engine/Engine/EngineService.h"
#include "Engine/Level/Actor.h"
#include "Engine/Level/SceneObject.h"
#include "Engine/Level/Prefabs/Prefab.h"
#include "Engine/Level/Prefabs/PrefabManager.h"
#include "Engine/Profiler/ProfilerCPU.h"
#include "Engine/Scripting/Script.h"
#include "Engine/Scripting/Scripting.h"
#include "Engine/Scripting/ScriptingObjectReference.h"
#include "Engine/Threading/Threading.h"
#include "Engine/Threading/ThreadLocal.h"

// Enables verbose logging for Network Replicator actions (dev-only)
#define NETWORK_REPLICATOR_DEBUG_LOG 1

#if NETWORK_REPLICATOR_DEBUG_LOG
#define NETWORK_REPLICATOR_LOG(messageType, format, ...) LOG(messageType, format, ##__VA_ARGS__)
#else
#define NETWORK_REPLICATOR_LOG(messageType, format, ...)
#endif

PACK_STRUCT(struct NetworkMessageObjectReplicate
    {
    NetworkMessageIDs ID = NetworkMessageIDs::ObjectReplicate;
    uint32 OwnerFrame;
    Guid ObjectId; // TODO: introduce networked-ids to synchronize unique ids as ushort (less data over network)
    Guid ParentId;
    char ObjectTypeName[128]; // TODO: introduce networked-name to synchronize unique names as ushort (less data over network)
    uint16 DataSize;
    });

PACK_STRUCT(struct NetworkMessageObjectSpawn
    {
    NetworkMessageIDs ID = NetworkMessageIDs::ObjectSpawn;
    Guid ObjectId;
    Guid ParentId;
    Guid PrefabId;
    Guid PrefabObjectID;
    uint32 OwnerClientId;
    char ObjectTypeName[128]; // TODO: introduce networked-name to synchronize unique names as ushort (less data over network)
    });

PACK_STRUCT(struct NetworkMessageObjectDespawn
    {
    NetworkMessageIDs ID = NetworkMessageIDs::ObjectDespawn;
    Guid ObjectId;
    });

PACK_STRUCT(struct NetworkMessageObjectRole
    {
    NetworkMessageIDs ID = NetworkMessageIDs::ObjectRole;
    Guid ObjectId;
    uint32 OwnerClientId;
    });

struct NetworkReplicatedObject
{
    ScriptingObjectReference<ScriptingObject> Object;
    Guid ObjectId;
    Guid ParentId;
    uint32 OwnerClientId;
    uint32 LastOwnerFrame = 0;
    NetworkObjectRole Role;
    uint8 Spawned = false;
#if NETWORK_REPLICATOR_DEBUG_LOG
    uint8 InvalidTypeWarn = false;
#endif

    bool operator==(const NetworkReplicatedObject& other) const
    {
        return Object == other.Object;
    }

    bool operator==(const ScriptingObject* other) const
    {
        return Object == other;
    }

    bool operator==(const Guid& other) const
    {
        return ObjectId == other;
    }

    String ToString() const
    {
        return ObjectId.ToString();
    }
};

inline uint32 GetHash(const NetworkReplicatedObject& key)
{
    return GetHash(key.ObjectId);
}

struct Serializer
{
    NetworkReplicator::SerializeFunc Methods[2];
    void* Tags[2];
};

namespace
{
    CriticalSection ObjectsLock;
    HashSet<NetworkReplicatedObject> Objects;
    Array<ScriptingObjectReference<ScriptingObject>> SpawnQueue;
    Array<Guid> DespawnQueue;
    Dictionary<Guid, Guid> IdsRemappingTable;
    NetworkStream* CachedWriteStream = nullptr;
    NetworkStream* CachedReadStream = nullptr;
    Array<NetworkClient*> NewClients;
    Array<NetworkConnection> CachedTargets;
    Dictionary<ScriptingTypeHandle, Serializer> SerializersTable;
}

class NetworkReplicationService : public EngineService
{
public:
    NetworkReplicationService()
        : EngineService(TEXT("Network Replication"), 1100)
    {
    }

    void Dispose() override;
};

void NetworkReplicationService::Dispose()
{
    NetworkInternal::NetworkReplicatorClear();
}

NetworkReplicationService NetworkReplicationServiceInstance;

void INetworkSerializable_Serialize(void* instance, NetworkStream* stream, void* tag)
{
    const int16 vtableOffset = (int16)tag;
    ((INetworkSerializable*)((byte*)instance + vtableOffset))->Serialize(stream);
}

void INetworkSerializable_Deserialize(void* instance, NetworkStream* stream, void* tag)
{
    const int16 vtableOffset = (int16)tag;
    ((INetworkSerializable*)((byte*)instance + vtableOffset))->Deserialize(stream);
}

NetworkReplicatedObject* ResolveObject(Guid objectId)
{
    auto it = Objects.Find(objectId);
    if (it != Objects.End())
        return &it->Item;
    IdsRemappingTable.TryGet(objectId, objectId);
    it = Objects.Find(objectId);
    return it != Objects.End() ? &it->Item : nullptr;
}

NetworkReplicatedObject* ResolveObject(Guid objectId, Guid parentId, char objectTypeName[128])
{
    // Lookup object
    NetworkReplicatedObject* obj = ResolveObject(objectId);
    if (obj)
        return obj;

    // Try to find the object within the same parent (eg. spawned locally on both client and server)
    IdsRemappingTable.TryGet(parentId, parentId);
    const ScriptingTypeHandle objectType = Scripting::FindScriptingType(StringAnsiView(objectTypeName));
    if (!objectType)
        return nullptr;
    for (auto& e : Objects)
    {
        auto& item = e.Item;
        const ScriptingObject* obj = item.Object.Get();
        if (item.LastOwnerFrame == 0 &&
            item.ParentId == parentId &&
            obj &&
            obj->GetTypeHandle() == objectType)
        {
            // Boost future lookups by using indirection
            NETWORK_REPLICATOR_LOG(Info, "[NetworkReplicator] Remap object ID={} into object {}:{}", objectId, item.ToString(), obj->GetType().ToString());
            IdsRemappingTable.Add(objectId, item.ObjectId);

            return &item;
        }
    }

    return nullptr;
}

void SendObjectSpawnMessage(const NetworkReplicatedObject& item, ScriptingObject* obj)
{
    NetworkMessageObjectSpawn msgData;
    msgData.ObjectId = item.ObjectId;
    msgData.ParentId = item.ParentId;
    const bool isClient = NetworkManager::IsClient();
    if (isClient)
    {
        // Remap local client object ids into server ids
        IdsRemappingTable.KeyOf(msgData.ObjectId, &msgData.ObjectId);
        IdsRemappingTable.KeyOf(msgData.ParentId, &msgData.ParentId);
    }
    msgData.PrefabId = Guid::Empty;
    msgData.PrefabObjectID = Guid::Empty;
    auto* objScene = ScriptingObject::Cast<SceneObject>(obj);
    if (objScene && objScene->HasPrefabLink())
    {
        msgData.PrefabId = objScene->GetPrefabID();
        msgData.PrefabObjectID = objScene->GetPrefabObjectID();
    }
    msgData.OwnerClientId = item.OwnerClientId;
    const StringAnsiView& objectTypeName = obj->GetType().Fullname;
    Platform::MemoryCopy(msgData.ObjectTypeName, objectTypeName.Get(), objectTypeName.Length());
    msgData.ObjectTypeName[objectTypeName.Length()] = 0;
    auto* peer = NetworkManager::Peer;
    NetworkMessage msg = peer->BeginSendMessage();
    msg.WriteStructure(msgData);
    if (isClient)
        peer->EndSendMessage(NetworkChannelType::ReliableOrdered, msg);
    else
        peer->EndSendMessage(NetworkChannelType::ReliableOrdered, msg, CachedTargets);
}

void SendObjectRoleMessage(const NetworkReplicatedObject& item, const NetworkClient* excludedClient = nullptr)
{
    NetworkMessageObjectRole msgData;
    msgData.ObjectId = item.ObjectId;
    msgData.OwnerClientId = item.OwnerClientId;
    auto peer = NetworkManager::Peer;
    NetworkMessage msg = peer->BeginSendMessage();
    msg.WriteStructure(msgData);
    if (NetworkManager::IsClient())
    {
        NetworkManager::Peer->EndSendMessage(NetworkChannelType::ReliableOrdered, msg);
    }
    else
    {
        CachedTargets.Clear();
        for (const NetworkClient* client : NetworkManager::Clients)
        {
            if (client->State == NetworkConnectionState::Connected && client != excludedClient)
                CachedTargets.Add(client->Connection);
        }
        peer->EndSendMessage(NetworkChannelType::ReliableOrdered, msg, CachedTargets);
    }
}

FORCE_INLINE void DeleteNetworkObject(ScriptingObject* obj)
{
    if (obj->Is<Script>() && ((Script*)obj)->GetParent())
        ((Script*)obj)->GetParent()->DeleteObject();
    else
        obj->DeleteObject();
}

SceneObject* FindPrefabObject(Actor* a, const Guid& prefabObjectId)
{
    if (a->GetPrefabObjectID() == prefabObjectId)
        return a;
    for (auto* script : a->Scripts)
    {
        if (script->GetPrefabObjectID() == prefabObjectId)
            return script;
    }
    SceneObject* result = nullptr;
    for (int32 i = 0; i < a->Children.Count() && !result; i++)
        result = FindPrefabObject(a->Children[i], prefabObjectId);
    return result;
}

#if !COMPILE_WITHOUT_CSHARP

#include "Engine/Scripting/ManagedCLR/MUtils.h"

void INetworkSerializable_Managed(void* instance, NetworkStream* stream, void* tag)
{
    auto signature = (Function<void(void*, void*)>::Signature)tag;
    signature(instance, stream);
}

void NetworkReplicator::AddSerializer(const ScriptingTypeHandle& typeHandle, const Function<void(void*, void*)>& serialize, const Function<void(void*, void*)>& deserialize)
{
    // This assumes that C# glue code passed static method pointer (via Marshal.GetFunctionPointerForDelegate)
    AddSerializer(typeHandle, INetworkSerializable_Managed, INetworkSerializable_Managed, (void*)*(SerializeFunc*)&serialize, (void*)*(SerializeFunc*)&deserialize);
}

#endif

void NetworkReplicator::AddSerializer(const ScriptingTypeHandle& typeHandle, SerializeFunc serialize, SerializeFunc deserialize, void* serializeTag, void* deserializeTag)
{
    if (!typeHandle)
        return;
    const Serializer serializer{ { serialize, deserialize }, { serializeTag, deserializeTag } };
    SerializersTable[typeHandle] = serializer;
}

bool NetworkReplicator::InvokeSerializer(const ScriptingTypeHandle& typeHandle, void* instance, NetworkStream* stream, bool serialize)
{
    if (!typeHandle || !instance || !stream)
        return true;

    // Get serializers pair from table
    Serializer serializer;
    if (!SerializersTable.TryGet(typeHandle, serializer))
    {
        // Fallback to INetworkSerializable interface (if type implements it)
        const ScriptingType& type = typeHandle.GetType();
        const ScriptingType::InterfaceImplementation* interface = type.GetInterface(INetworkSerializable::TypeInitializer);
        if (interface)
        {
            serializer.Methods[0] = INetworkSerializable_Serialize;
            serializer.Methods[1] = INetworkSerializable_Deserialize;
            serializer.Tags[0] = serializer.Tags[1] = (void*)interface->VTableOffset; // Pass VTableOffset to the callback
            SerializersTable.Add(typeHandle, serializer);
        }
        else if (const ScriptingTypeHandle baseTypeHandle = typeHandle.GetType().GetBaseType())
        {
            // Fallback to base type
            return InvokeSerializer(baseTypeHandle, instance, stream, serialize);;
        }
        else
            return true;
    }

    // Invoke serializer
    const byte idx = serialize ? 0 : 1;
    serializer.Methods[idx](instance, stream, serializer.Tags[idx]);
    return false;
}

void NetworkReplicator::AddObject(ScriptingObject* obj, ScriptingObject* parent)
{
    if (!obj || NetworkManager::State == NetworkConnectionState::Offline)
        return;
    ScopeLock lock(ObjectsLock);
    if (Objects.Contains(obj))
        return;

    // Automatic parenting for scene objects
    if (!parent)
    {
        auto sceneObject = ScriptingObject::Cast<SceneObject>(obj);
        if (sceneObject)
            parent = sceneObject->GetParent();
    }

    // Add object to the list
    NetworkReplicatedObject item;
    item.Object = obj;
    item.ObjectId = obj->GetID();
    item.ParentId = parent ? parent->GetID() : Guid::Empty;
    item.OwnerClientId = NetworkManager::ServerClientId; // Server owns objects by default
    item.Role = NetworkManager::IsClient() ? NetworkObjectRole::Replicated : NetworkObjectRole::OwnedAuthoritative;
    NETWORK_REPLICATOR_LOG(Info, "[NetworkReplicator] Add new object {}:{}, parent {}:{}", item.ToString(), obj->GetType().ToString(), item.ParentId.ToString(), parent ? parent->GetType().ToString() : String::Empty);
    Objects.Add(MoveTemp(item));
}

void NetworkReplicator::SpawnObject(ScriptingObject* obj)
{
    if (!obj || NetworkManager::State == NetworkConnectionState::Offline)
        return;
    ScopeLock lock(ObjectsLock);
    const auto it = Objects.Find(obj->GetID());
    if (it != Objects.End() && it->Item.Spawned)
        return; // Skip if object was already spawned

    // Register for spawning (batched during update)
    SpawnQueue.AddUnique(obj);
}

void NetworkReplicator::DespawnObject(ScriptingObject* obj)
{
    if (!obj || NetworkManager::State == NetworkConnectionState::Offline)
        return;
    ScopeLock lock(ObjectsLock);
    const auto it = Objects.Find(obj->GetID());
    if (it == Objects.End())
        return;
    auto& item = it->Item;
    if (item.Object != obj || !item.Spawned || item.OwnerClientId != NetworkManager::LocalClientId)
        return;

    // Register for despawning (batched during update)
    const Guid id = obj->GetID();
    ASSERT_LOW_LAYER(!DespawnQueue.Contains(id));
    DespawnQueue.Add(id);

    // Prevent spawning
    SpawnQueue.Remove(obj);

    // Delete object locally
    DeleteNetworkObject(obj);
}

uint32 NetworkReplicator::GetObjectClientId(ScriptingObject* obj)
{
    uint32 id = 0;
    if (obj)
    {
        ScopeLock lock(ObjectsLock);
        const auto it = Objects.Find(obj->GetID());
        if (it != Objects.End())
            id = it->Item.OwnerClientId;
    }
    return id;
}

NetworkObjectRole NetworkReplicator::GetObjectRole(ScriptingObject* obj)
{
    NetworkObjectRole role = NetworkObjectRole::None;
    if (obj)
    {
        ScopeLock lock(ObjectsLock);
        const auto it = Objects.Find(obj->GetID());
        if (it != Objects.End())
            role = it->Item.Role;
    }
    return role;
}

void NetworkReplicator::SetObjectOwnership(ScriptingObject* obj, uint32 ownerClientId, NetworkObjectRole localRole)
{
    if (!obj)
        return;
    ScopeLock lock(ObjectsLock);
    const auto it = Objects.Find(obj->GetID());
    if (it == Objects.End())
        return;
    auto& item = it->Item;
    if (item.Object != obj)
        return;

    // Check if this client is object owner
    if (item.OwnerClientId == NetworkManager::LocalClientId)
    {
        // Check if object owner will change
        if (item.OwnerClientId != ownerClientId)
        {
            // Change role locally
            CHECK(localRole != NetworkObjectRole::OwnedAuthoritative);
            item.OwnerClientId = ownerClientId;
            item.LastOwnerFrame = 1;
            item.Role = localRole;
            SendObjectRoleMessage(item);
        }
        else
        {
            // Object is the owner
            CHECK(localRole == NetworkObjectRole::OwnedAuthoritative);
        }
    }
    else
    {
        // Allow to change local role of the object (except ownership)
        CHECK(localRole != NetworkObjectRole::OwnedAuthoritative);
        item.Role = localRole;
    }
}

void NetworkReplicator::DirtyObject(ScriptingObject* obj)
{
    ScopeLock lock(ObjectsLock);
    const auto it = Objects.Find(obj->GetID());
    if (it == Objects.End())
        return;
    auto& item = it->Item;
    if (item.Object != obj || item.Role != NetworkObjectRole::OwnedAuthoritative)
        return;
    // TODO: implement objects state replication frequency and dirtying
}

void NetworkInternal::NetworkReplicatorClientConnected(NetworkClient* client)
{
    ScopeLock lock(ObjectsLock);
    NewClients.Add(client);
}

void NetworkInternal::NetworkReplicatorClientDisconnected(NetworkClient* client)
{
    ScopeLock lock(ObjectsLock);
    NewClients.Remove(client);
}

void NetworkInternal::NetworkReplicatorClear()
{
    ScopeLock lock(ObjectsLock);

    // Cleanup
    NETWORK_REPLICATOR_LOG(Info, "[NetworkReplicator] Shutdown");
    for (auto it = Objects.Begin(); it.IsNotEnd(); ++it)
    {
        auto& item = it->Item;
        ScriptingObject* obj = item.Object.Get();
        if (obj && item.Spawned)
        {
            // Cleanup any spawned objects
            DeleteNetworkObject(obj);
        }
    }
    Objects.Clear();
    Objects.SetCapacity(0);
    SpawnQueue.Clear();
    DespawnQueue.Clear();
    IdsRemappingTable.Clear();
    SAFE_DELETE(CachedWriteStream);
    SAFE_DELETE(CachedReadStream);
    NewClients.Clear();
    CachedTargets.Resize(0);
}

void NetworkInternal::NetworkReplicatorPreUpdate()
{
    // Inject ObjectsLookupIdMapping to properly map networked object ids into local object ids (deserialization with Scripting::TryFindObject will remap objects)
    Scripting::ObjectsLookupIdMapping.Set(&IdsRemappingTable);
}

void NetworkInternal::NetworkReplicatorUpdate()
{
    PROFILE_CPU();
    ScopeLock lock(ObjectsLock);
    if (Objects.Count() == 0)
        return;
    if (CachedWriteStream == nullptr)
        CachedWriteStream = New<NetworkStream>();
    const bool isClient = NetworkManager::IsClient();
    NetworkStream* stream = CachedWriteStream;
    NetworkPeer* peer = NetworkManager::Peer;
    // TODO: introduce NetworkReplicationHierarchy to optimize objects replication in large worlds (eg. batched culling networked scene objects that are too far from certain client to be relevant)
    // TODO: per-object sync interval (in frames) - could be scaled by hierarchy (eg. game could slow down sync rate for objects far from player)

    if (!isClient && NewClients.Count() != 0)
    {
        // Sync any previously spawned objects with late-joining clients
        PROFILE_CPU_NAMED("NewClients");
        // TODO: try iterative loop over several frames to reduce both server and client perf-spikes in case of large amount of spawned objects
        CachedTargets.Clear();
        for (NetworkClient* client : NewClients)
            CachedTargets.Add(client->Connection);
        for (auto it = Objects.Begin(); it.IsNotEnd(); ++it)
        {
            auto& item = it->Item;
            ScriptingObject* obj = item.Object.Get();
            if (!obj || !item.Spawned)
                continue;
            SendObjectSpawnMessage(item, obj);
        }
        NewClients.Clear();
    }

    // Collect clients for replication (from server)
    CachedTargets.Clear();
    for (const NetworkClient* client : NetworkManager::Clients)
    {
        if (client->State == NetworkConnectionState::Connected)
            CachedTargets.Add(client->Connection);
    }
    if (!isClient && CachedTargets.Count() == 0)
    {
        // Early exit if server has nobody to send data to
        Scripting::ObjectsLookupIdMapping.Set(nullptr);
        return;
    }

    // Despawn
    if (DespawnQueue.Count() != 0)
    {
        PROFILE_CPU_NAMED("DespawnQueue");
        for (const Guid& e : DespawnQueue)
        {
            // Send despawn message
            NETWORK_REPLICATOR_LOG(Info, "[NetworkReplicator] Despawn object ID={}", e.ToString());
            NetworkMessageObjectDespawn msgData;
            msgData.ObjectId = e;
            if (isClient)
            {
                // Remap local client object ids into server ids
                IdsRemappingTable.KeyOf(msgData.ObjectId, &msgData.ObjectId);
            }
            NetworkMessage msg = peer->BeginSendMessage();
            msg.WriteStructure(msgData);
            if (isClient)
                peer->EndSendMessage(NetworkChannelType::ReliableOrdered, msg);
            else
                peer->EndSendMessage(NetworkChannelType::ReliableOrdered, msg, CachedTargets);
        }
        DespawnQueue.Clear();
    }

    // Spawn
    if (SpawnQueue.Count() != 0)
    {
        PROFILE_CPU_NAMED("SpawnQueue");
        for (ScriptingObjectReference<ScriptingObject>& e : SpawnQueue)
        {
            ScriptingObject* obj = e.Get();
            auto it = Objects.Find(obj->GetID());
            if (it == Objects.End())
            {
                // Ensure that object is added to the replication locally
                NetworkReplicator::AddObject(obj);
                it = Objects.Find(obj->GetID());
            }
            if (it == Objects.End())
                continue; // Skip deleted objects
            auto& item = it->Item;
            if (item.OwnerClientId != NetworkManager::LocalClientId || item.Role != NetworkObjectRole::OwnedAuthoritative)
                continue; // Skip spawning objects that we don't own

            NETWORK_REPLICATOR_LOG(Info, "[NetworkReplicator] Spawn object ID={}", item.ToString());
            SendObjectSpawnMessage(item, obj);
            item.Spawned = true;
        }
        SpawnQueue.Clear();
    }

    if (isClient)
    {
        // TODO: client logic to replicate owned objects to the server
    }
    else
    {
        // Brute force synchronize all networked objects with clients
        for (auto it = Objects.Begin(); it.IsNotEnd(); ++it)
        {
            auto& item = it->Item;
            ScriptingObject* obj = item.Object.Get();
            if (!obj)
            {
                // Object got deleted
                NETWORK_REPLICATOR_LOG(Info, "[NetworkReplicator] Remove object {}, owned by {}", item.ToString(), item.ParentId.ToString());
                Objects.Remove(it);
                continue;
            }

            // Serialize object
            stream->Initialize();
            const bool failed = NetworkReplicator::InvokeSerializer(obj->GetTypeHandle(), obj, stream, true);
            if (failed)
            {
#if NETWORK_REPLICATOR_DEBUG_LOG
                if (!item.InvalidTypeWarn)
                {
                    item.InvalidTypeWarn = true;
                    NETWORK_REPLICATOR_LOG(Error, "[NetworkReplicator] Cannot serialize object {} of type {} (missing serialization logic)", item.ToString(), obj->GetType().ToString());
                }
#endif
                continue;
            }

            // Send object to clients
            {
                const uint32 size = stream->GetPosition();
                ASSERT(size <= MAX_uint16)
                NetworkMessageObjectReplicate msgData;
                msgData.OwnerFrame = NetworkManager::Frame;
                msgData.ObjectId = item.ObjectId;
                msgData.ParentId = item.ParentId;
                const StringAnsiView& objectTypeName = obj->GetType().Fullname;
                Platform::MemoryCopy(msgData.ObjectTypeName, objectTypeName.Get(), objectTypeName.Length());
                msgData.ObjectTypeName[objectTypeName.Length()] = 0;
                msgData.DataSize = size;
                // TODO: split object data (eg. more messages) if needed
                NetworkMessage msg = peer->BeginSendMessage();
                msg.WriteStructure(msgData);
                msg.WriteBytes(stream->GetBuffer(), size);
                // TODO: per-object relevancy for connected clients (eg. skip replicating actor to far players)
                peer->EndSendMessage(NetworkChannelType::Unreliable, msg, CachedTargets);

                // TODO: stats for bytes send per object type
            }
        }
    }

    // Clear networked objects mapping table
    Scripting::ObjectsLookupIdMapping.Set(nullptr);
}

void NetworkInternal::OnNetworkMessageObjectReplicate(NetworkEvent& event, NetworkClient* client, NetworkPeer* peer)
{
    NetworkMessageObjectReplicate msgData;
    event.Message.ReadStructure(msgData);
    ScopeLock lock(ObjectsLock);
    NetworkReplicatedObject* e = ResolveObject(msgData.ObjectId, msgData.ParentId, msgData.ObjectTypeName);
    if (e)
    {
        auto& item = *e;
        ScriptingObject* obj = item.Object.Get();
        if (!obj)
            return;

        // Reject event from someone who is not an object owner
        if (client && item.OwnerClientId != client->ClientId)
            return;

        // Skip replication if we own the object (eg. late replication message after ownership change)
        if (item.Role == NetworkObjectRole::OwnedAuthoritative)
            return;

        // Drop object replication if it has old data (eg. newer message was already processed due to unordered channel usage)
        if (item.LastOwnerFrame >= msgData.OwnerFrame)
            return;
        item.LastOwnerFrame = msgData.OwnerFrame;

        // Setup message reading stream
        if (CachedReadStream == nullptr)
            CachedReadStream = New<NetworkStream>();
        NetworkStream* stream = CachedReadStream;
        stream->Initialize(event.Message.Buffer + event.Message.Position, msgData.DataSize);

        // Deserialize object
        const bool failed = NetworkReplicator::InvokeSerializer(obj->GetTypeHandle(), obj, stream, false);
        if (failed)
        {
#if NETWORK_REPLICATOR_DEBUG_LOG
            if (failed && !item.InvalidTypeWarn)
            {
                item.InvalidTypeWarn = true;
                NETWORK_REPLICATOR_LOG(Error, "[NetworkReplicator] Cannot serialize object {} of type {} (missing serialization logic)", item.ToString(), obj->GetType().ToString());
            }
#endif
        }

        // TODO: speed up replication of client-owned object to other clients from server
    }
    else
    {
        // TODO: put message to the queue to be resolved later (eg. object replication came before spawn packet) - use TTL to prevent memory overgrowing
    }
}

void NetworkInternal::OnNetworkMessageObjectSpawn(NetworkEvent& event, NetworkClient* client, NetworkPeer* peer)
{
    NetworkMessageObjectSpawn msgData;
    event.Message.ReadStructure(msgData);
    ScopeLock lock(ObjectsLock);
    NetworkReplicatedObject* e = ResolveObject(msgData.ObjectId, msgData.ParentId, msgData.ObjectTypeName);
    if (e)
    {
        auto& item = *e;
        item.Spawned = true;
        if (NetworkManager::IsClient())
        {
            // Server always knows the best so update ownership of the existing object
            item.OwnerClientId = msgData.OwnerClientId;
            if (item.Role == NetworkObjectRole::OwnedAuthoritative)
                item.Role = NetworkObjectRole::Replicated;
        }
        else if (item.OwnerClientId != msgData.OwnerClientId)
        {
            // Other client spawned object with a different owner
            // TODO: send reply message to inform about proper object ownership that client
        }
    }
    else
    {
        // Recreate object locally
        ScriptingObject* obj = nullptr;
        const NetworkReplicatedObject* parent = ResolveObject(msgData.ParentId);
        if (msgData.PrefabId.IsValid())
        {
            Actor* prefabInstance = nullptr;
            Actor* parentActor = parent && parent->Object && parent->Object->Is<Actor>() ? parent->Object.As<Actor>() : nullptr;
            if (parentActor && parentActor->GetPrefabID() == msgData.PrefabId)
            {
                // Reuse parent object as prefab instance
                prefabInstance = parentActor;
            }
            else if (parentActor = Scripting::TryFindObject<Actor>(msgData.ParentId))
            {
                // Try to find that spawned prefab (eg. prefab with networked script was spawned before so now we need to link it)
                for (Actor* child : parentActor->Children)
                {
                    if (child->GetPrefabID() == msgData.PrefabId)
                    {
                        if (Objects.Contains(child->GetID()))
                        {
                            obj = FindPrefabObject(child, msgData.PrefabObjectID);
                            if (Objects.Contains(obj->GetID()))
                            {
                                // Other instance with already spawned network object
                                obj = nullptr;
                            }
                            else
                            {
                                // Reuse already spawned object within a parent
                                prefabInstance = child;
                                break;
                            }
                        }
                    }
                }
            }
            if (!prefabInstance)
            {
                // Spawn prefab
                auto prefab = (Prefab*)LoadAsset(msgData.PrefabId, Prefab::TypeInitializer);
                if (!prefab)
                {
                    NETWORK_REPLICATOR_LOG(Error, "[NetworkReplicator] Failed to find prefab {}", msgData.PrefabId.ToString());
                    return;
                }
                prefabInstance = PrefabManager::SpawnPrefab(prefab, nullptr, nullptr);
                if (!prefabInstance)
                {
                    NETWORK_REPLICATOR_LOG(Error, "[NetworkReplicator] Failed to spawn object type {}", msgData.PrefabId.ToString());
                    return;
                }
            }
            if (!obj)
                obj = FindPrefabObject(prefabInstance, msgData.PrefabObjectID);
            if (!obj)
            {
                NETWORK_REPLICATOR_LOG(Error, "[NetworkReplicator] Failed to find object {} in prefab {}", msgData.PrefabObjectID.ToString(), msgData.PrefabId.ToString());
                Delete(prefabInstance);
                return;
            }
        }
        else
        {
            // Spawn object
            const ScriptingTypeHandle objectType = Scripting::FindScriptingType(StringAnsiView(msgData.ObjectTypeName));
            obj = ScriptingObject::NewObject(objectType);
            if (!obj)
            {
                NETWORK_REPLICATOR_LOG(Error, "[NetworkReplicator] Failed to spawn object type {}", String(msgData.ObjectTypeName));
                return;
            }
        }
        if (!obj->IsRegistered())
            obj->RegisterObject();

        // Add object to the list
        NetworkReplicatedObject item;
        item.Object = obj;
        item.ObjectId = obj->GetID();
        item.ParentId = parent ? parent->ObjectId : Guid::Empty;
        item.OwnerClientId = client ? client->ClientId : NetworkManager::ServerClientId;
        item.Role = NetworkObjectRole::Replicated;
        item.Spawned = true;
        NETWORK_REPLICATOR_LOG(Info, "[NetworkReplicator] Add new object {}:{}, parent {}:{}", item.ToString(), obj->GetType().ToString(), item.ParentId.ToString(), parent ? parent->Object->GetType().ToString() : String::Empty);
        Objects.Add(MoveTemp(item));

        // Boost future lookups by using indirection
        NETWORK_REPLICATOR_LOG(Info, "[NetworkReplicator] Remap object ID={} into object {}:{}", msgData.ObjectId, item.ToString(), obj->GetType().ToString());
        IdsRemappingTable.Add(msgData.ObjectId, item.ObjectId);

        // Automatic parenting for scene objects
        auto sceneObject = ScriptingObject::Cast<SceneObject>(obj);
        if (sceneObject)
        {
            if (parent && parent->Object.Get() && parent->Object->Is<Actor>())
                sceneObject->SetParent(parent->Object.As<Actor>());
            else if (auto* parentActor = Scripting::TryFindObject<Actor>(msgData.ParentId))
                sceneObject->SetParent(parentActor);
        }

        // TODO: if  we're server then spawn this object further on other clients
    }
}

void NetworkInternal::OnNetworkMessageObjectDespawn(NetworkEvent& event, NetworkClient* client, NetworkPeer* peer)
{
    NetworkMessageObjectDespawn msgData;
    event.Message.ReadStructure(msgData);
    ScopeLock lock(ObjectsLock);
    NetworkReplicatedObject* e = ResolveObject(msgData.ObjectId);
    if (e)
    {
        auto& item = *e;
        ScriptingObject* obj = item.Object.Get();
        if (!obj || !item.Spawned)
            return;

        // Reject event from someone who is not an object owner
        if (client && item.OwnerClientId != client->ClientId)
            return;

        // Remove object
        Objects.Remove(obj);
        DeleteNetworkObject(obj);
    }
    else
    {
        NETWORK_REPLICATOR_LOG(Error, "[NetworkReplicator] Failed to despawn object {}", msgData.ObjectId);
    }
}

void NetworkInternal::OnNetworkMessageObjectRole(NetworkEvent& event, NetworkClient* client, NetworkPeer* peer)
{
    NetworkMessageObjectRole msgData;
    event.Message.ReadStructure(msgData);
    ScopeLock lock(ObjectsLock);
    NetworkReplicatedObject* e = ResolveObject(msgData.ObjectId);
    if (e)
    {
        auto& item = *e;
        ScriptingObject* obj = item.Object.Get();
        if (!obj)
            return;

        // Reject event from someone who is not an object owner
        if (client && item.OwnerClientId != client->ClientId)
            return;

        // Update
        item.OwnerClientId = msgData.OwnerClientId;
        item.LastOwnerFrame = 1;
        if (item.OwnerClientId == NetworkManager::LocalClientId)
        {
            // Upgrade ownership automatically
            item.Role = NetworkObjectRole::OwnedAuthoritative;
            item.LastOwnerFrame = 0;
        }
        else if (item.Role == NetworkObjectRole::OwnedAuthoritative)
        {
            // Downgrade ownership automatically
            item.Role = NetworkObjectRole::Replicated;
        }
        if (!NetworkManager::IsClient())
        {
            // Server has to broadcast ownership message to the other clients
            SendObjectRoleMessage(item, client);
        }
    }
    else
    {
        NETWORK_REPLICATOR_LOG(Error, "[NetworkReplicator] Unknown object role update {}", msgData.ObjectId);
    }
}
