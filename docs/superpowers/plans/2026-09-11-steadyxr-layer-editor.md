# SteadyXR layer editor implementation plan

> For agentic workers: use superpowers:subagent-driven-development to implement this plan task by task.

Goal: ship SteadyXR 0.7.0 with a usable, tested OpenXR registry editor and preserved preview compatibility.

Architecture: existing native C++ preview launches a separate Windows Forms editor on demand; bundled C# handles raw registry snapshots, checked writes and verified restoration. No GPU capture or full preview elevation inside the editor.

Tech stack: existing C++17/Win32/D3D11, Windows PowerShell 5.1, Windows Forms, .NET Framework C#, Windows native registry APIs.

Spec: docs/superpowers/specs/2026-09-11-steadyxr-layer-editor-design.md

## Global constraints

- English application UI, SteadyXR branding; preserve legacy DLL/IPC technical identifiers.
- No live OpenXR/SteamVR/OBS registration changes during development, tests or delivery. Tests use unique HKCU fixture keys and ignored files.
- Preserve previous packages and settings; deliver to the existing Horizon Lock VR OneDrive root.
- All edits stage until Apply; durable backup, conflict check, write verification and failure restoration required. Never delete vendor files.

## Task 1: Registry store with verified writes

Files: preview/layers/RegistryStore.cs and preview/layers/RegistryStore.Tests.ps1.

Interfaces: namespace SteadyXR; mutable LayerValue(Name:string, Kind:uint, Data:byte[]) and LayerGroup(Hive:string, Is64Bit:bool, SubKey:string, Exists:bool, Values:List<LayerValue>, Error:string), Clone methods. RegistryStore(basePath:string, userOnly:bool) defaults to production OpenXR ApiLayers root, public Read(bool is64Bit), Clone(List<LayerGroup>), Same(List<LayerGroup>,List<LayerGroup>), Apply(before,after,backupDirectory):string backupPath, ReadBackup(path):List<LayerGroup>. Read returns four groups in implicit HKLM/HKCU, explicit HKLM/HKCU order (two groups in fixture userOnly mode).

- [x] Write private-key failing regression for ordered raw values, arbitrary value types and enable/delete/reorder. Observe failure with initial stub.
- [x] Implement checked P/Invoke read/write and durable unique backup JSON. Apply validates identities and conflicts, verifies full contents/order and reports restoration failures. Reordering uses verified flat-key replacement with preserved security when necessary; no OS atomicity claim.
- [x] Test round trip, reorder, disable, removal, missing groups, Unicode, unknown/binary values, external-change conflict, access/write/backup failures and undo. Confirm no production key writes.

## Task 2: Editor and native integration

Files: preview/layers/LayerEditor.ps1, editor tests; preview/main.cpp, connection helpers/tests, CMake and package script.

- [x] Build staged list UI with registry snapshots, full details and honest status; default64bit, explicit/user/system groups, up/down restrictions.
- [x] Add refresh/discard, enable/disable/remove, register own bundled layer, apply/undo, admin-only editor relaunch and restart-game notice. Preserve draft if an apply fails.
- [x] Add OpenXR layers button launching only bundled script with Windows PowerShell STA/hidden console. Refresh connection status on return. Read-only diagnostics + hidden fixture UI tests.
- [x] Package script/source and C# files beside app; add standalone Start-Layer-Editor.cmd convenience launcher.

## Task 3: Branding, verification and delivery

- [x] Rename visible application titles/theme/docs and archive labels to SteadyXR 0.7.0; retain legacy technical DLL/IPC identifiers and migrated settings/presets.
- [x] Run meaningful editor registry/UI tests and complete existing CTest suite. Inspect editor and main UI captures. Verify capture layer binary equality.
- [x] Independent whole-change review and findings fixed.
- [ ] Package/checksums/source verification and local integration; no remote publish or live registration edits.
