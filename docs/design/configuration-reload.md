# Configuration reload

This note records the reload guarantees that maintainers must preserve. The
user-facing configuration guide describes only the outcomes needed while
editing a configuration.

## Core contract

A reload is transactional. Umbriel commits a new configuration only after the
complete file set parses and validates successfully.

If a reload fails:

- The last valid configuration remains active.
- The live compositor state remains unchanged.
- Diagnostics describe the failed attempt.
- Watches follow the attempted include graph. Fixing a broken included file can
  therefore trigger recovery without another edit to the main file.

A successful reload applies only the runtime effects caused by changed
sections. Saving identical content is inert: it must not reset output state,
move focus, close overlays, or otherwise disturb the session.

## Selective runtime effects

Output state and workspace inventory are independent effects.

- Changing mode, scale, transform, or position reapplies output state.
- Changing an output's `workspaces` value reconciles its workspace inventory
  and refreshes workspace layout.
- Changing only layout settings refreshes workspace geometry without
  reconciling the inventory or reapplying output state.
- Changing total border width refreshes window decoration and workspace layout
  because borders contribute to resolved tile spacing.
- `general.autostart` commands run only during startup, never during reload.
- `general.xwayland` changes require a compositor restart.

A section can affect more than one runtime consumer. Keep those dependencies
explicit when adding configuration fields, rather than falling back to a full
session refresh.

## Verification

The relevant regression coverage is in:

- [`tests/config_change.cpp`](../../tests/config_change.cpp), which checks
  change classification and runtime effects.
- [`tests/harness/checks/050_config_reload.sh`](../../tests/harness/checks/050_config_reload.sh),
  which checks inert reloads, selective layout updates, border dependencies,
  and recovery after an included file fails to parse.
