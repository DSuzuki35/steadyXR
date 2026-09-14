# OpenXR overlay capture 0.9.0

Authorized goal: selectable additional OpenXR composition layers in both standalone and OBS. Capture supported quad, cylinder and color projection layers; list unsupported types explicitly. Preserve the original headset submission and existing 20-second A/B behavior. Disabled by default. All/None mode persists; individual slot selections apply only to the current producer session, avoiding accidental inclusion of a different overlay after restart. UI labels identify type, dimensions and slot; no invented vendor attribution.

## Architecture
IPC v3 isolates the expanded metadata from old binaries, requiring explicit connection upgrade and game restart. Shared catalog has 16 session-stable slots; frames contain at most 8 selected overlay images. Consumer owns selection changes. Producer resolves spaces at displayTime and publishes each overlay pose relative to the main context-eye pose. It copies private released swapchain snapshots, including static-image reuse, into keyed shared textures. Renderer acquires all selected textures atomically with frame metadata and draws overlays in submission order with correct alpha on the stabilized or original ray for each A/B panel. No per-frame CPU image readback.

## Work
1. Root: transport structs/validation/selection ownership tests, release integration/0.9 packaging/docs.
2. Capture implementation: layer.cpp+native layer tests; correct space conversion, eye selection, static snapshots, bounded catalog/resources, unsupported reporting, no application/runtime state mutation.
3. GPU implementation: renderer.cpp+renderer tests; keyed ownership and retained metadata, ray/quad/cylinder/projection hit tests and alpha blending, original/A-B behavior.
4. UI implementation: main.cpp+OBS source/settings+tests; layer checkboxes, All/None, session-scoped saved selection, explicit OBS refresh without synchronous/periodic property rebuild.
5. Review, native tests, build with one 30-day stamp, deliver complete 0.9 ZIP/source/checksums and app-adjacent OBS folders preserving previous releases and current INI. No automatic OpenXR/OBS configuration mutations.

## Performance follow-up
User requested a measured resource pass. Cache the successful private snapshot revision and array slice per shared overlay output, skipping only unchanged overlay publication copies. Retain all legal release snapshots and keyed ownership transfers; base images still copy. Native tests count actual GPU queue call sites in the test build and verify pixel freshness and reconnect behavior. App/OBS scheduling audit found existing hidden/paused capture release and change-driven drawing; no unmeasured cadence change.
