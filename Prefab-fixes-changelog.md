# Prefab-fixes Branch — Bug Fix & Change Documentation

Changes between `derelict-mods` and `Prefab-fixes` — 25 files changed, +498 / -111 lines.

---

## Overview

| # | Bug / Change | Category | Severity |
|---|-------------|----------|----------|
| 1 | [Prefab Sync Cascade: Stack Overflow, Cyclic Parents, and Stale ID Mappings](#1-prefab-sync-cascade-stack-overflow-cyclic-parents-and-stale-id-mappings) | Prefab | Critical |
| 2 | [Silent Material Fallback Masks Broken or Cleared References](#2-silent-material-fallback-masks-broken-or-cleared-references) | Asset / Rendering | High |
| 3 | [Cleared GUID Overrides on Prefab Instances Don't Survive Save/Load](#3-cleared-guid-overrides-on-prefab-instances-dont-survive-saveload) | Serialization | High |
| 4 | [Enum Serialization Uses Integers — Fragile to Reordering](#4-enum-serialization-uses-integers--fragile-to-reordering) | Serialization | High |
| 5 | [Asset Cache Evicts Entries for File-Locked Assets](#5-asset-cache-evicts-entries-for-file-locked-assets) | Asset | High |
| 6 | [Asset Reload Race Condition With In-Flight Loading Task](#6-asset-reload-race-condition-with-in-flight-loading-task) | Asset | Medium |
| 7 | [Corrupted GUID Hex Strings Silently Produce Garbage Values](#7-corrupted-guid-hex-strings-silently-produce-garbage-values) | Serialization | Medium |
| 8 | [Managed Virtual Method Wrapper Crashes on Missing Scripts](#8-managed-virtual-method-wrapper-crashes-on-missing-scripts) | Build / Interop | Medium |
| 9 | [No Way to Reset or Relink a Broken Prefab Instance](#9-no-way-to-reset-or-relink-a-broken-prefab-instance) | Editor (New Feature) | — |
| 10 | [Insufficient Diagnostic Context in Warnings](#10-insufficient-diagnostic-context-in-warnings) | Logging | Low |
| 11 | [Editor Window Layout Lost on Cache Clear](#11-editor-window-layout-lost-on-cache-clear) | Editor | Low |
| 12 | [Skeleton Node Visualization Stuck On After Tab Switch](#12-skeleton-node-visualization-stuck-on-after-tab-switch) | Editor | Low |

---

## 1. Prefab Sync Cascade: Stack Overflow, Cyclic Parents, and Stale ID Mappings

**TL;DR:** Modifying a nested prefab crashes the editor with a stack overflow in `GetLocalToWorldMatrix`. Three interrelated causes — stale ID mappings create cyclic parent chains, no visited-set allows infinite sync recursion, and world-space matrix computation fires during sync when the hierarchy is unsafe. Fixed with three layers of defense.

### What the user sees

Modify and save a nested prefab. The engine enters prefab sync. The editor hangs and crashes with a stack overflow. The call stack shows thousands of frames of `Actor::GetLocalToWorldMatrix` calling itself through `_parent` pointers.

### The three root causes

**A) IdsMapping leaks stale entries between prefab instances**

When the engine syncs prefab instances, it maintains a lookup table (IdsMapping) that translates IDs from the prefab template to IDs in your scene. This table should be reset for each instance, but it wasn't — entries from one instance bled into the next. This caused objects to be reparented to the wrong actor. In nested prefab scenarios with root changes, this creates **transient cyclic parent references** (actor A's parent is B, B's parent is A).

**B) No visited-set in `SyncNestedPrefabs` — infinite recursion at the prefab level**

The old `SyncNestedPrefabs` iterated the same `allPrefabs` list on every recursive call with no tracking of what had already been synced. In a diamond dependency (A nests B and C, both nest D), D would be synced twice. In a cycle (A nests B, B nests A — possible through indirect chains), the recursion never terminates.

**C) `AnimatedModel::PreInitSkinningData` triggers world-space matrix computation during sync**

During prefab sync, deserializing an `AnimatedModel` can trigger `OnSkinnedModelLoaded` (line 997) → `PreInitSkinningData` (line 154) → `UpdateBounds` (line 869) → `GetLocalToWorldMatrix`. This function walks up the `_parent` chain recursively. If the parent chain contains a cycle (from cause A), this recurses infinitely and overflows the stack.

### The full crash chain

```
Modify nested prefab
  → Prefab::Apply()
    → SyncNestedPrefabs() [no visited set — may re-enter or infinite-loop]
      → SyncChangesInternal() on referencing prefabs
        → ApplyAllInternal()
          → Deserialize each SceneObject
            → SetupIdsMapping [leaks entries from previous iterations]
            → AnimatedModel::Deserialize()
              → DESERIALIZE(SkinnedModel) sets asset reference
              → OnSkinnedModelLoaded() fires synchronously (asset already in memory)
                → PreInitSkinningData()
                  → UpdateBounds()
                    → GetLocalToWorldMatrix() walks up _parent chain
                      → _parent has cyclic reference from stale IdsMapping
                        → STACK OVERFLOW
```

### The fixes (three layers of defense)

**Layer 1: RAII guard isolates IdsMapping per instance** — Prevents the root cause (cyclic parents from stale mappings).

| File | Lines | What Changed |
|------|-------|-------------|
| `ISerializeModifier.h` | 34-50 | New `IdsMappingScope` RAII struct — snapshots `IdsMapping` on construction, restores on destruction |
| `Prefab.Apply.cpp` | 398 | `IdsMappingScope` guard before per-instance `SetupIdsMapping` + `Deserialize` in sync loop |
| `Prefab.Apply.cpp` | 433-447 | Rebuild `IdsMapping` from scratch after `ObjectsRemovalService::Flush()` — purges entries pointing to freed memory |
| `Prefab.Apply.cpp` | 466 | `IdsMappingScope` guard before restoring per-instance local overrides |
| `Prefab.Apply.cpp` | 1064 | `IdsMappingScope` guard in `ApplyAllInternal` deserialization loop |
| `Prefab.Apply.cpp` | 315 | Added `continue` after null root-object error log — previously fell through to null dereference |

**Layer 2: Visited-set in `SyncNestedPrefabs`** — Prevents infinite recursion at the prefab cascade level.

| File | Lines | What Changed |
|------|-------|-------------|
| `Prefab.h` | 8, 106 | `SyncNestedPrefabs` signature gains `HashSet<Guid>* syncedIds = nullptr` |
| `Prefab.Apply.cpp` | 1406-1444 | Creates local `HashSet<Guid>` on first call, seeds with current ID. Checks/inserts before each sync. Passes the set through recursive calls so the entire cascade shares one visited-set. |

**Layer 3: Skip world-space operations during sync** — Cuts off the matrix stack overflow even if causes A or B somehow recur.

| File | Lines | What Changed |
|------|-------|-------------|
| `AnimatedModel.cpp` | 196-199 | `UpdateBounds()` and `UpdateSockets()` wrapped in `if (GetScene() \|\| !_parent)` — skipped when actor has a parent but no scene (the mid-sync state). Bounds are computed later in `BeginPlay` (line 830). |

---

## 2. Silent Material Fallback Masks Broken or Cleared References

**Symptom:** User clears a material slot on a model, but the model keeps showing its default material as if nothing happened. Also: if a material reference is broken or still loading, it silently shows the default instead of flagging the problem.

**Root Cause:** `GetMaterial` in `StaticModel`, `AnimatedModel`, and `SplineModel` checked `if (!material)` and fell back unconditionally. No distinction between "never assigned" and "assigned but missing/loading."

**The Fix:** Fallback to the default material now only happens when no material was ever assigned. If you assigned a material and it's missing or broken, the engine returns null instead of silently substituting — making the problem visible so you can fix it.

This required a paired change to `AssetReference` (see below): the engine now remembers the assigned ID immediately, even while the asset is still loading. Without that, a loading material would look identical to "no material assigned" and the fallback would still trigger incorrectly.

| File | Lines | What Changed |
|------|-------|-------------|
| `StaticModel.cpp` | 118 | Added `Entries[materialSlotIndex].Material.GetID() == Guid::Empty` guard before fallback |
| `StaticModel.cpp` | 587 | Same guard on the `entryIndex` overload |
| `AnimatedModel.cpp` | 1337 | Same guard |
| `SplineModel.cpp` | 361 | Same guard |

**Paired fix — `AssetReference._id` cache:**

`AssetReferenceBase::GetID()` was `return _asset ? _asset->GetID() : Guid::Empty`. During async load, the GUID was invisible. A new `_id` field caches the GUID immediately on assignment, before the async load starts.

| File | Lines | What Changed |
|------|-------|-------------|
| `AssetReference.h` | 15-18 | New `Guid _id` field on `AssetReferenceBase`, with comment explaining why |
| `AssetReference.h` | 60-62 | `GetID()` returns `_id` instead of `_asset->GetID()` |
| `AssetReference.h` | 178 | `operator=(Guid)` sets `_id = id` before calling `OnSet(LoadAsset(...))` |
| `Asset.cpp` | 91 | `OnSet(Asset*)` syncs `_id` from the asset when the pointer is set directly |

---

## 3. Cleared GUID Overrides on Prefab Instances Don't Survive Save/Load

**Symptom:** You clear a material or asset reference on a prefab instance in your scene. Save, reload — the field silently reverts to whatever the prefab default is, every time. Your edit is permanently lost on every save cycle.

**Root Cause:** `ShouldSerialize(Guid)` was `return v.IsValid()`. `Guid::Empty` is not "valid," so the cleared override was never written to the prefab diff. On load, the prefab default (non-empty) was restored.

**The Fix:** Compare against the prefab default value instead of checking validity. An empty GUID that differs from a non-empty default is now serialized.

| File | Lines | What Changed |
|------|-------|-------------|
| `Serialization.cpp` | 402 | Changed from `return v.IsValid()` to `return otherObj ? (v != *(Guid*)otherObj) : true` |

---

## 4. Enum Serialization Uses Integers — Fragile to Reordering

**Symptom:** If enum members are reordered, inserted, or renumbered between builds, all saved data silently loads incorrect values. Any scene, prefab, or settings file using enums is affected.

**Root Cause:** Both C++ (`Serialization.h`) and C# (`Newtonsoft.Json`) serialized enums as raw integers.

**The Fix:** Enums now serialize as string names in both C++ and C#. **Existing project files are fully backward compatible** — deserialization accepts both strings and integers. Old files auto-upgrade to string names on next save.

**Additional fix found during review:** The original C++ `ScriptingEnum::FromString` silently returned `(T)0` when a string didn't match any known member — no warning, no indication of data loss. If a developer renames an enum member (e.g., `ModeC` → `ModeCharlie`), every saved file with `"ModeC"` would silently become 0. The deserializer now logs a warning identifying the unrecognized value so the developer knows which files need updating. (Note: C# side already throws `JsonSerializationException` on mismatch — this only affected the C++ path.) A proper rename/migration tool (analogous to Unity's `[FormerlySerializedAs]`) would fully solve this but is out of scope for this branch.

| File | Lines | What Changed |
|------|-------|-------------|
| `Serialization.h` | 230-246 | C++ `Serialize<enum>` writes string name via `ScriptingEnum::GetName`; `Deserialize<enum>` tries int parse first (compat), then `FromString` |
| `Serialization.h` | 245-256 | Enum name lookup now warns on unrecognized string values instead of silently defaulting to 0 |
| `JsonSerializer.cs` | 207 | Added `StringEnumConverter` to default Newtonsoft.Json settings |
| `ExtendedDefaultContractResolver.cs` | 51-53 | Removed override that forced enum dictionary keys to integers |

---

## 5. Asset Cache Evicts Entries for File-Locked Assets

**Symptom:** After a `git pull`, `git checkout`, or while an antivirus scanner is running, assets randomly go missing in the editor — materials turn pink, textures vanish, prefab references break. Restarting the editor usually fixes it (after a slow rediscovery scan), but the problem comes back on the next git operation.

**Not permanent data loss:** Your scene and prefab files are unaffected — the GUID references are intact on disk. The engine just can't find the asset files temporarily. Everything recovers once the lock releases and the cache rebuilds.

**The worst case:** Editor starts while git is mid-operation (pull, checkout, merge). Every recently-touched `.flax` file may be locked. The old code discards **all** of them during startup — mass eviction affecting dozens or hundreds of assets at once.

**Root Cause:** `AssetsCache::IsEntryValid` returned a boolean. A file that exists but can't be opened (locked) returned `false`, same as a truly missing file.

**The Fix:** A three-state enum (`Valid` / `Invalid` / `Inaccessible`) replaces the boolean. Inaccessible entries are kept with their cached metadata. A one-time warning is logged per entry.

| File | Lines | What Changed |
|------|-------|-------------|
| `AssetsCache.h` | 43-58 | New `EntryValidation` enum with `Valid`, `Invalid`, `Inaccessible` |
| `AssetsCache.h` | 87, 98 | `WarnedInaccessible` bool on `Entry` to prevent log spam |
| `AssetsCache.h` | 263 | `IsEntryValid` return type changed to `EntryValidation` |
| `AssetsCache.cpp` | 585-650 | `IsEntryValid` returns `Inaccessible` when file exists but storage can't open; clears `WarnedInaccessible` when file becomes accessible again |
| `AssetsCache.cpp` | 116-117 | `Init` keeps `Inaccessible` entries instead of evicting them |
| `AssetsCache.cpp` | 299-310, 332-343 | Both `FindAsset` overloads: only remove on `Invalid`; keep and log one-time warning on `Inaccessible` |

---

## 6. Asset Reload Race Condition With In-Flight Loading Task

**Symptom:** Observed on materials. After reimporting or reloading an asset, the material occasionally shows stale data or the engine crashes. This is intermittent and timing-dependent — more likely on large assets that take a long time to load.

**What's happening:** When you reimport or reload an asset, the engine starts a new loading task. But if the previous load was still in-flight (e.g., it timed out after 30s), the old task can finish after the new one starts. The old task writes stale data over the fresh reload, or crashes with an assertion failure.

**Root Cause:** `Reload()` did not cancel the in-flight loading task. The `onLoad` callback did not verify it was the correct task. The old task could acquire the lock after Reload and call `loadAsset()` against the torn-down/re-initializing state.

**Original fix:** `Reload` atomically clears `_loadingTask` to disown the old task. `onLoad` re-checks `_loadingTask` after acquiring the lock — if cleared, the stale task bails out.

**Remaining gap found during review:** The original guard checked `_loadingTask == 0`, but not whether it points to *this* task. If `Reload()` clears `_loadingTask` and starts a new load (setting it to the new task pointer), the stale task sees `_loadingTask != 0` and passes the guard. It then calls `loadAsset()`, writing stale data directly into asset member fields (every `loadAsset` implementation writes directly — no staging buffer), and clears `_loadingTask` to 0, orphaning the new task. The new task's `onLoad` sees `_loadingTask == 0` and bails — the reload silently fails.

**Complete fix:** Both guards in `onLoad` now compare `_loadingTask` against the current task pointer (`(intptr)task`) rather than checking for zero. Only the correct task proceeds to `loadAsset()`.

| File | Lines | What Changed |
|------|-------|-------------|
| `Asset.cpp` | 432 | `Reload` clears `_loadingTask` via `AtomicStore(&_loadingTask, 0)` after acquiring lock |
| `Asset.cpp` | 609 | Pre-lock guard: `_loadingTask != (intptr)task` — rejects stale tasks even when a new task exists |
| `Asset.cpp` | 616 | Post-lock guard: `_loadingTask != (intptr)task` — authoritative identity check under lock prevents stale `loadAsset()` |

---

## 7. Corrupted GUID Hex Strings Silently Produce Garbage Values

**Symptom:** After resolving a git merge conflict in a scene or prefab file, objects go missing or reference the wrong assets with no error message. The corrupted GUID from the merge conflict was silently parsed as garbage.

**Root Cause:** `Serialization::Deserialize(Guid)` called `StringUtils::ParseHex` four times without checking return values.

**The Fix:** Each hex parse is checked. On failure, GUID is set to `Guid::Empty` and a warning is logged.

| File | Lines | What Changed |
|------|-------|-------------|
| `Serialization.cpp` | 430-437 | Added `failed` flag accumulating `ParseHex` results; on failure, logs warning and sets `v = Guid::Empty` |

---

## 8. Managed Virtual Method Wrapper Crashes on Missing Scripts

**Symptom:** You rename or delete a C# script class, or a compilation error prevents it from loading. The engine crashes immediately with no useful error message — just an assertion in generated code. The debugger gives you no information about which script is broken or which object references it. A routine code change shouldn't crash the editor.

**Root Cause:** Generated C++ wrappers used `ASSERT(scriptVTable && scriptVTable[N])` and did not null-check `GetOrCreateManagedInstance()`. An ASSERT is for internal invariant violations, not user-recoverable conditions like a missing script type.

**The Fix:** Replace `ASSERT` with null-checks that log a one-time warning including the type name, object name, and ID — giving the user the information they need to find and fix the broken reference. The engine continues running.

| File | Lines | What Changed |
|------|-------|-------------|
| `BindingsGenerator.Cpp.cs` | 1526-1527 | `scriptVTable` null-check with once-per-type warning instead of `ASSERT` |
| `BindingsGenerator.Cpp.cs` | 1542-1543 | `GetOrCreateManagedInstance()` null-check with once-per-type warning |
| `BindingsGenerator.Cpp.cs` | 1605, 1611, 1641 | Hoisted managed instance into local var `managedInstance`, reused across thunk/invoke paths |

---

## 9. No Way to Reset or Relink a Broken Prefab Instance

*New Feature — not a bug fix.*

**Problem:** A prefab instance in your scene gets corrupted — bad overrides stacked up, or it lost its link to the source prefab. Previously the only option was to manually fix every property or delete and re-create the instance by hand.

**What Was Added:**
- **Reset Prefab** (scene & prefab editor): Deletes the instance, spawns a fresh copy from the prefab, preserves transform/name/sibling order.
- **Relink to Prefab** (prefab editor): Asset picker lets the user choose a prefab, then replaces the selected actor with a fresh instance.
- Both labeled "(no undo)" in the UI. Removed auto-save on relink so users can review before committing.
- **Confirmation dialogs added during review:** All three methods (`ResetPrefab`, `ResetNestedPrefab`, `RelinkToPrefab`) now show a `MessageBox` confirmation before executing, since a misclick would permanently destroy all instance overrides with no way to recover.

| File | Lines | What Changed |
|------|-------|-------------|
| `PrefabsModule.cs` | 239, 254 | New `ResetPrefab()` method with confirmation dialog |
| `PrefabWindow.Actions.cs` | 318, 328, 396, 420 | New `ResetNestedPrefab()` and `RelinkToPrefab()` methods with confirmation dialogs; removed auto-save from relink |
| `PrefabWindow.Hierarchy.cs` | 337, 340 | Context menu items "Reset Prefab (no undo)" and "Relink to Prefab... (no undo)" |
| `SceneTreeWindow.ContextMenu.cs` | 151 | "Reset Prefab (no undo)" in scene tree context menu |

---

## 10. Insufficient Diagnostic Context in Warnings

**Symptom:** The log says "material parameter not found" or "missing object data in prefab" but doesn't say which actor, which scene, or which prefab instance triggered it. In a large project with hundreds of instances, these warnings are useless without context.

**The Fix:** All relevant warnings now include the full context chain — which scene, which prefab, which actor — so you can find and fix the problem.

| File | Lines | What Changed |
|------|-------|-------------|
| `MaterialBase.cpp` | 6, 27-29, 45-47 | Missing-parameter warnings now include `LogContext` chain |
| `SceneObjectsFactory.cpp` | 9, 168, 264, 295, 598, 867 | "Missing object data in prefab" warnings include `LogContext` |
| `Prefab.cpp` | 64 | "Cannot instantiate from not loaded prefab" now includes prefab path and ID |

---

## 11. Editor Window Layout Lost on Cache Clear

**Symptom:** You clear the project cache (a common troubleshooting step) and all your editor panel arrangements are gone.

**Root Cause:** `WindowsLayout.xml` was stored in `Globals.ProjectCacheFolder`.

**The Fix:** Moved to `Globals.ProjectFolder`. Added to `.gitignore`.

| File | Lines | What Changed |
|------|-------|-------------|
| `WindowsModule.cs` | 760 | Path changed from `ProjectCacheFolder` to `ProjectFolder` |
| `.gitignore` | 19 | Added `WindowsLayout.xml` |

---

## 12. Skeleton Node Visualization Stuck On After Tab Switch

**Symptom:** In the SkinnedModel editor, switching to the Skeleton tab shows bone visualization. Switching away leaves the nodes visible.

**Root Cause:** The handler only set `ShowNodes = true` on enter, never `false` on leave.

**The Fix:** `_preview.ShowNodes = tabs.SelectedTab is SkeletonTab` — evaluates to `false` on all other tabs.

| File | Lines | What Changed |
|------|-------|-------------|
| `SkinnedModelWindow.cs` | 609 | Conditional expression replaces one-way `if` |
