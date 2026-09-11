# Preview controls, vertical offset and complete presets

User requests: fix disappearing/buggy sliders and ineffective Y offset; add built-in presets and user presets containing all settings. Existing render extension is confirmed working by user.

Evidence: live v2 frame right eye5166x2900, optical aspect1.850063,16:9 output. Current Y only uses post-reserve aspect slack, which is zero. Current trackbar customdraw erases full client in PREPAINT but only some items redraw on partial native paints.

Plan:
1. Reproduce/fix trackbar repaint using a complete, clipped draw at PREPAINT, retain native input and add regression for thumb-only/incremental callbacks. Check parent clipping, slider UI persistence callbacks and avoid disk writes during every drag motion.
2. Y offset moves final output within original captured vertical bounds, including uniform/per-edge/framing reserves, keeping zoom constant and zero-value legacy output. Apply before final correction clipping; cover GPU and geometry tests for wide-aspect feeds like the observed frame. Both capture modes.
3. Native Presets window: built-in Natural / Stream Comfort / Strong Leveling adjust tuning, retain source/output. Named custom presets snapshot all persisted app configuration including render settings. Store user snapshots in sibling Presets folder shared across versions; no arbitrary filesystem names. Explicit overwrite/delete inUI, atomicwrites, tests for save/load/delete/roundtrip and malformed names. Restore live preview values and saved OpenXR values for next game launch, never automatically restartgame or changeactive runtime.
4. Deliver0.6.1 with existing0.6 layer compatibility (v2), preserve user's activeconfiguration and previousreleases. Nativeui,geometry,GPU,fullsuite,review,packagechecksums. No game restart needed for UI/Y/presets; applyingchangedrenderrequeststillrequiresgame restart.

Work distribution per subagent-driven-development: theme agent owns theme and painting regressions; preset agent owns snapshotstore and native preset dialog; root owns Ygeometry, mainUIintegration,persistence,migration,release.

Completed: native trackbar repaint fix and parent child clipping; Y framing fix in both capture paths; debounced automatic writes; three built-in tuning profiles; complete custom INI presets including render drafts and stable source identity; compatible registered-layer directory lookup and explicit render save routing. The OpenXR capture layer binary remains identical to 0.6.0.

Verification: all 20 CTest groups passed, including native hidden-window preset round trips, read-only failure handling, alternate registered render INI, slider repeated/partial repaint, composition geometry and D3D11 rendering. UI captures were inspected. Independent behavior review and scoped follow-up passed after fixing unchecked settings writes before snapshot creation.
