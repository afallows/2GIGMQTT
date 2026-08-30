# GC2 security dashboard

`gc2_security_dashboard.yaml` is a complete Lovelace raw configuration built
around the entities created by the `gc2_panel` custom integration.

`full_dashboard_with_gc2_security.yaml` is the user's complete existing Home
Assistant dashboard with the same Security view and card templates already
merged. Paste that file into the raw configuration editor when replacing the
whole dashboard.

## What it does

- Finds the GC2 alarm and zones by integration, so it does not mix in old
  RTL_433 entities with similar names.
- Groups panel-programmed zones into doors, windows, motion, life safety, and
  water/environment sections.
- Removes the MAC-qualified GC2 device prefix from dashboard labels while
  retaining that identifier in Home Assistant's device registry.
- Shows battery, RF supervision, tamper, bypass, and trouble state on each zone
  card using the attributes already supplied by the integration.
- Elevates open/active and unhealthy zones into an Attention section.
- Lets an Attention-zone card bypass or restore its physical GC2 zone after an
  explicit confirmation; bypassed zones remain in Attention until restored.
- Presents a full-width alarm overview with live totals for active, bypassed,
  and unhealthy zones plus dedicated Stay, Away, and Disarm actions.
- Shows meaningful alarm and zone state changes in Home Assistant's native
  Activity timeline. Supervisory-only updates and unavailable/available
  recovery transitions are omitted.
- Includes panel diagnostics and a GC2 chime/announcement volume preset
  dropdown for 0%, 10%, 25%, 50%, 75%, and 100%.
- Retains the locks, water shutoff, and lighting controls from the previous
  dashboard.

The zone lists are dynamic. New or renamed panel zones appear after the GC2
integration receives the updated panel inventory; the dashboard itself does
not need to be edited.

## Requirements

Install these two frontend cards from HACS:

1. **Button Card** (`custom-cards/button-card`)
2. **Auto Entities** (`thomasloven/lovelace-auto-entities`)

HACS normally registers their dashboard resources automatically. Refresh the
browser after installing them.

## Install as a new UI-managed dashboard

1. In Home Assistant, open **Settings > Dashboards** and create a dashboard.
2. Open the new dashboard, select **Edit dashboard**, then open the three-dot
   menu and choose **Raw configuration editor**.
3. Replace the raw configuration with the contents of
   `gc2_security_dashboard.yaml` and save. To retain the supplied existing
   dashboard pages, use `full_dashboard_with_gc2_security.yaml` instead.
4. Refresh the page once so the custom cards load.

The view preserves the two Home Assistant user IDs from the previous YAML. Use
the visual editor's **Visibility** tab to add or remove users. Visibility is
presentation, not an authorization boundary; Home Assistant user permissions
remain the security control.

## Merge into an existing raw dashboard

Copy both of these pieces:

- the top-level `button_card_templates` block; and
- the `Security` item under `views`.

If the existing dashboard already has `button_card_templates`, merge the GC2
templates into that existing map instead of adding a second top-level key.

## Site-specific controls

GC2 alarm, zone, health, and volume entities are selected automatically. The
four locks/water controls and four lighting controls at the bottom intentionally
use the entity IDs from the previous dashboard. Edit or remove those tiles if
their entity IDs change.

## Visual language

- Blue/teal: armed or normal live status
- Amber: open/active or intentionally bypassed
- Red: alarm, tamper, supervision loss, low battery, or panel trouble
- Grey health icon: the panel has not reported that condition yet

Every zone card has three compact health icons for battery, RF supervision,
and tamper. A fourth shield-off icon appears while a zone is bypassed. Selecting
a zone opens its full Home Assistant details and history.
