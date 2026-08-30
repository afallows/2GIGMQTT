# 2GIGMQTT GC2 Bridge

Arduino firmware for a **Waveshare ESP32-S3-Zero** that listens to an
authorized 2GIG GC2 debug console, exposes it to one authenticated Tera Term
client on TCP port **4444**, and publishes conditioned panel data to MQTT.
The included Home Assistant custom integration turns that data into a complete
front end for the physical GC2 rather than creating a second alarm engine.

## Current behavior

- Opens an intentionally password-free captive setup network only when the
  bridge has no saved configuration.
- Saves Wi-Fi, Tera Term, and MQTT settings in ESP32 nonvolatile storage.
- Requires a separate 8-to-64-character password before a Telnet client can
  access the console. Only a salted, iterated hash of that password is stored.
- Begins passive UART baud detection 120 seconds after power-up and retries
  until it sees usable panel traffic. Hardware measurements only create an
  ordered candidate list. Each candidate must then decode recognizable panel
  text before it becomes the active baud.
- Defaults to **monitor mode** after baud detection. It unlocks the debug
  console, verifies it every 5 minutes, and automatically unlocks it again if
  needed. It reads live zone state and trouble memory every 30 seconds, panel
  security status every 60 seconds, and retained alarm memory after UART startup,
  network recovery, and every 5 minutes. It
  sends the larger read-only `zone_info 1` programming query after startup and
  every 6 hours.
- Decodes the panel's programmed `VoiceDesc` names and updates the retained
  MQTT inventory if a zone is renamed, enabled, disabled, or reprogrammed.
- Publishes zone activity, panel state, backup-battery status, firmware data,
  normalized panel/zone trouble conditions, alarm memory, RF/Z-Wave
  diagnostics, and transient events to MQTT.
- Exposes the GC2 chime and announcement sounder volume as both a synchronized
  Home Assistant fine-control slider and a preset dropdown, so automations can
  lower it overnight and restore it later.
- Uses a retained MQTT Last Will so Home Assistant marks the GC2 entities
  unavailable if the ESP32 or network disappears.

The current zone-name vocabulary is for the GC2 **EN_US** firmware family,
including the bench panel's V1.24 build. Unknown vocabulary tokens are ignored;
the zone number and type remain available as fallbacks. Live chime playback
labels are used only when no programmed name can be decoded; their internal
`chime_N-` prefix is never included in a Home Assistant entity name.

## Hardware

Default pin assignments are in `AppConfig.h`:

| ESP32-S3-Zero | Connect to GC2 |
|---|---|
| GPIO5 (ESP RX) | Panel TX |
| GPIO6 (ESP TX) | Panel RX |
| GND | Panel ground |

GPIO5/GPIO6 use UART1, so boot messages cannot reach the panel. The USB
connector remains on GPIO19/GPIO20. GPIO0 remains the BOOT/factory-reset
button, and GPIO21 drives the onboard WS2812 RGB status LED.

The status LED is solid red when the bridge has not been provisioned. After
Wi-Fi, bridge-password, and MQTT settings have been saved, it flashes blue at
500 ms intervals. The blue indication means settings are provisioned; it does
not by itself guarantee that Wi-Fi or MQTT is currently connected.

The ESP32-S3 pins are **3.3 V only**. Verify the panel header voltage before
connecting it. Never connect RS-232 or 5 V signaling directly to the board.

## Arduino IDE setup

1. Install **ESP32 by Espressif Systems** version 3.3.5 or newer in Boards
   Manager.
2. Install **PubSubClient by Nick O'Leary** version 2.8 from Library Manager.
3. Select **Waveshare ESP32-S3-Zero** as the board.
4. Set **USB CDC On Boot** to **Enabled**.
5. Open `2GIGMQTT.ino`, compile, and upload.
6. Open Serial Monitor at 115200 baud for status messages.

## Authenticated OTA updates

After this firmware has been installed once by USB, later sketches can be
uploaded over the normal Wi-Fi network directly from Arduino IDE. OTA is not
started on the open provisioning access point. It uses port **3232**, advertises
the bridge's MAC-derived hostname, and requires the same 8-to-64-character
bridge password used by Tera Term. Only a SHA-256 password hash is retained for
OTA authentication.

For a newly provisioned bridge, OTA is ready as soon as Wi-Fi connects. A
bridge that was already provisioned before OTA support was installed does not
yet have the required SHA-256 hash: connect to port 4444 and successfully enter
the existing bridge password once. Within a few seconds Serial Monitor shows:

```text
[ota] Arduino IDE network upload ready as gc2-bridge-aabbccddeeff.local on port 3232
```

To upload:

1. Keep the computer and bridge on the same local network.
2. In Arduino IDE, open **Tools > Port** and select the network port named
   `gc2-bridge-aabbccddeeff`.
3. Click **Upload** and enter the bridge password when requested.
4. Leave power connected until the upload completes and the bridge reboots.

Saved Wi-Fi, bridge, and MQTT settings survive a successful OTA update. Holding
BOOT for the documented factory-reset interval erases the OTA credential along
with the other saved settings. If the network port is absent, first confirm the
`[ota] ... ready` message, then disable any VPN and allow local mDNS and TCP/UDP
port 3232 through the computer's firewall.

## Provisioning

When no saved configuration exists, Serial Monitor shows a temporary network:

```text
[wifi] Network:  GC2-Bridge-AABBCCDDEEFF
[wifi] Password: none (open network)
```

Join it and use the captive page. If the page does not appear automatically,
open `http://192.168.4.1/`. Configure:

- the normal Wi-Fi network and password;
- an 8-to-64-character Tera Term bridge password;
- MQTT broker hostname/IP and port;
- optional MQTT username and password;
- base topic, normally `2gig/gc2`;
- Home Assistant GC2-integration discovery, normally enabled.

The setup access point shuts down after the station connection succeeds.
Once settings have been saved, losing Wi-Fi never reopens the setup access
point: the bridge remains provisioned and continues retrying the saved network.
Hold BOOT for the documented factory reset to erase settings and authorize a
new provisioning session.

## Tera Term and console modes

Create a TCP/IP connection using Telnet, host
`gc2-bridge-aabbccddeeff.local` (or the IP shown in Serial Monitor), port
**4444**, with local echo off. The 12 hexadecimal digits are that ESP's
factory-programmed Wi-Fi station MAC. Enter the bridge password when prompted.

The ESP32 handles these local commands and never forwards them verbatim:

| Command | Action |
|---|---|
| `/monitor` | Automatic-control monitor mode. Blocks arbitrary typed panel commands, resumes persistent debug-unlock supervision, and schedules a zone-programming refresh. |
| `/listen on` | Strictly passive mode. Clears queued input and blocks **all** UART transmissions, including unlock checks and metadata polls. |
| `/listen off` | Maintenance mode. Allows authenticated typed commands and resumes persistent debug-unlock supervision. |
| `/unlock` | Immediately schedules the automatic debug-unlock sequence in monitor or maintenance mode. |
| `/baud auto` | Stop the current UART and immediately restart automatic baud detection. |
| `/baud 115200` | Immediately set the UART to the supplied hardware-valid baud rate. Any valid integer rate is accepted. |

Normal typed panel commands are allowed only in maintenance mode and are paced
at one command per second. Panel output continues to stream in every mode.
The bridge starts in Monitor mode. It is safe from arbitrary typed maintenance
input, but it is not zero-TX: it maintains debug access, requests zone
programming, and can issue allow-listed MQTT control commands. Use `/listen on`
whenever the ESP32 must remain completely passive; MQTT control is rejected
until `/monitor` or `/listen off` restores transmission.

GPIO6 starts and remains electrically high-impedance while listening and while
baud candidates are validated. For a deliberate monitor or maintenance
command, the bridge attaches the UART transmitter only for the command bytes,
waits for the final stop bit, and immediately releases GPIO6 back to an input.
This prevents the UART's normal driven-high idle state from continuously
holding off a 345 MHz receiver that shares the panel line. `/listen on` also
releases GPIO6 immediately. If GPIO6 is wired directly against the receiver's
push-pull UART output, intentional writes still require electrical isolation or
a separate console input; two push-pull outputs must not drive one wire.

Automatic baud validation is fully passive. Trial bytes are not forwarded to
Tera Term, parsed into MQTT, or allowed to trigger `zone_info` polling. A baud
is accepted only after it produces a mostly printable line containing generic
panel evidence such as firmware/build information, EVENT, sensor/zone output,
battery data, subsystem logging, or a correctly shaped console timestamp. The
match does not depend on an exact firmware version, programmed zone name, or
sensor identifier. Automatic trials run in the observed-panel order **19200,
115200, then 38400**, with each candidate receiving a full 60-second passive
window unless recognizable text validates it sooner. Garbage never causes an
early switch. Other hardware-valid rates remain available through `/baud` and
the MQTT recovery control, but are not included in automatic detection.

## MQTT topic layout

Each board derives its setup SSID, network hostname, and stable MQTT device ID
from the full factory-programmed ESP Wi-Fi station MAC. With the default base
topic, topics resemble:

```text
2gig/gc2/gc2_bridge_aabbccddeeff/availability
2gig/gc2/gc2_bridge_aabbccddeeff/manifest
2gig/gc2/gc2_bridge_aabbccddeeff/panel/state
2gig/gc2/gc2_bridge_aabbccddeeff/panel/status
2gig/gc2/gc2_bridge_aabbccddeeff/panel/security
2gig/gc2/gc2_bridge_aabbccddeeff/panel/trouble
2gig/gc2/gc2_bridge_aabbccddeeff/panel/trouble/00
2gig/gc2/gc2_bridge_aabbccddeeff/panel/alarm_memory
2gig/gc2/gc2_bridge_aabbccddeeff/panel/set
2gig/gc2/gc2_bridge_aabbccddeeff/panel/bypass/set
2gig/gc2/gc2_bridge_aabbccddeeff/panel/sounder_volume
2gig/gc2/gc2_bridge_aabbccddeeff/panel/sounder_volume/set
2gig/gc2/gc2_bridge_aabbccddeeff/panel/command_status
2gig/gc2/gc2_bridge_aabbccddeeff/panel/battery
2gig/gc2/gc2_bridge_aabbccddeeff/panel/firmware
2gig/gc2/gc2_bridge_aabbccddeeff/uart/baud
2gig/gc2/gc2_bridge_aabbccddeeff/uart/baud/set
2gig/gc2/gc2_bridge_aabbccddeeff/zone/02/state
2gig/gc2/gc2_bridge_aabbccddeeff/user/001/state
2gig/gc2/gc2_bridge_aabbccddeeff/diagnostic
2gig/gc2/gc2_bridge_aabbccddeeff/event
```

The manifest, availability, state, zone, and observed-user topics are retained.
The `event` topic is not retained. The manifest identifies the transport schema,
full MAC-derived device ID, selected root topic, bridge version, and supported
capabilities. This lets Home Assistant reconstruct the panel even if it was not
running during the GC2's initial programming poll.

A zone state payload includes `state` (`ON`, `OFF`, or `UNKNOWN`), programmed
name, zone type, enabled/input fields, RF ID, raw panel values, last-seen
uptime, per-zone `battery_known`/`battery_low` fields, and separate bypass
fields. It also contains normalized `trouble_active`, `tamper`, and
`supervision_lost` fields. The bridge initializes state from the panel's read-only `zones` query,
then conditions live RF packets and open/restore messages. A bypassed open
sensor remains `ON`; the bridge never hides an open contact by reporting it
closed.

`panel/trouble` contains retained aggregate counts. Each populated
`panel/trouble/NN` topic contains the GC2 trouble-memory slot, zone, description,
active/restored state, acknowledgement state, and panel ticks. Empty slots are
deleted after each complete poll. `panel/security` is conditioned from the
read-only `panel_status` command and reports AC loss, panel/siren tamper, RF
jamming, communication failures, and reset-required state. The similarly named
GC2 simulator commands are never used.

`panel/alarm_memory` recovers the panel's retained alarm-memory flags after the
UART starts and after network connectivity returns. It is refreshed every five
minutes so an ESP32 or Home Assistant restart does not silently discard a
latched GC2 alarm indication.

An observed-user payload contains the numeric GC2 user ID, its last reported
action, origin, and bridge uptime. User `0` is the GC2 remote/system actor seen
with Alarm.com operations; positive IDs are panel users. Keypad codes and other
credentials are never published. The integration adds new user-ID entities as
they appear in normal panel activity.

`panel/state` is the retained Home Assistant alarm state: `disarmed`, `arming`,
`armed_home`, or `armed_away`. `panel/status` adds the detected mode, origin,
panel user, raw arming flags, and change time. Publish one of the following
non-retained payloads to `panel/set`:

```text
ARM_HOME
ARM_AWAY
DISARM
```

After baud detection, the bridge obtains the panel's daily debug unlock and
keeps that session available. It verifies `debug_lock_state` with `panel_misc`
every 5 minutes and runs the full unlock sequence again if the panel reports
locked; a failed attempt is retried after 30 seconds. MQTT control can therefore
send its single allow-listed command (`arm_stay 0`, `arm_away 0`, or `disarm 0`)
as soon as unlock is confirmed, without a per-command unlock/relock delay. The
result is retained on `panel/command_status`. `submitted` means the panel
command was transmitted; the actual result is determined only from subsequent
console state output. Strict `/listen on` mode rejects MQTT control and pauses
unlock supervision.

Physical zone bypass uses `panel/bypass/set` with a non-retained JSON payload:

```json
{"zone":2,"bypassed":true}
```

`false` requests an unbypass. Only zones 1 through 74 are accepted. Each
request waits for the shared persistent unlock if necessary, then maps to the
panel's literal `bypass <zone>` or `unbypass <zone>` command. Stale retained
alarm, bypass, and volume commands are deleted before the bridge subscribes.

Chime and announcement volume uses the GC2 console's normalized
`sounder_volume 0-100` control. Publish an integer from `0` through `100` to
the non-retained `panel/sounder_volume/set` topic. The bridge sends only that
allow-listed command after debug access is confirmed. It does not update the
retained state from the echoed request: it waits for an applied
`master_volume` trace or a live sounder/chime line. The read-only
`sounder_status` command is polled after a change and every 30 seconds, but its
separate transient output-channel volume is deliberately ignored.
On restart the bridge clears a stale retained volume until a fresh physical
panel value is observed, so Home Assistant cannot present yesterday's value as
current.
Strict `/listen on` mode rejects volume control along with the other MQTT
controls.

The retained `uart/baud` topic reports `detecting` or the active numeric baud.
The bridge mirrors that applied value as a retained message on `uart/baud/set`
as well. Publish `auto` or any hardware-valid integer baud to the `/set` topic
to recover or override the connection remotely. External commands should not
be retained; the bridge clears stale commands when connecting and then restores
the current applied value.
The Home Assistant integration exposes the applied UART baud and persistent
debug-unlock state as diagnostic entities. Manual baud recovery remains
available by publishing to the `/set` topic.

Serial Monitor prints each candidate as it is tested and the recognizable line
that validates the selected rate. If a panel uses a different rate, set it
directly using Tera Term or MQTT and use `/baud auto` later to retry the normal
three-rate sequence.

Firmware upgrading from the original direct MQTT-discovery releases
automatically deletes those retained Home Assistant entities. The custom
integration then owns the alarm panel, zones, bypass controls, and diagnostics,
preventing duplicate alarm entities.

## Home Assistant GC2 integration

The project includes `custom_components/gc2_panel` in the root layout required
by HACS. It treats the
GC2 as the alarm engine and Home Assistant as its front end. After one bridge
topic is selected it subscribes to that complete topic tree and automatically
creates:

- one `alarm_control_panel` for the physical GC2;
- one `binary_sensor` for every active programmed zone, using the programmed
  name and appropriate device class;
- one diagnostic low-battery binary sensor for every zone;
- zone trouble, tamper, and supervision-loss binary sensors;
- panel trouble, tamper, siren tamper, RF-jam, AC-loss, communication-failure,
  reset-required, and latched-alarm-memory binary sensors;
- one physical bypass switch for every zone;
- confirmation-protected bypass and restore actions from dashboard Attention
  cards, matched to the correct bridge and zone number;
- one fine-control volume slider from 0% through 100% and a dashboard-friendly
  preset dropdown for 0%, 10%, 25%, 50%, 75%, and 100%;
- panel battery, firmware, UART, debug-unlock, command-status, and console
  diagnostic sensors;
- a diagnostic sensor for every GC2 user ID observed in panel activity.

Install it through HACS by adding
`https://github.com/afallows/2GIGMQTT` as a custom **Integration** repository.
For a manual installation, copy `custom_components/gc2_panel` into Home
Assistant's `/config/custom_components/` directory and restart Home Assistant.
MQTT must already be configured in Home Assistant. With integration discovery enabled in
the bridge portal, Home Assistant shows the MAC-identified GC2 under
**Settings > Devices & services > Discovered**. Select it and confirm its MQTT
topic. If discovery was disabled, choose **Add Integration > 2GIG GC2 Panel**
and enter the full topic, for example
`2gig/gc2/gc2_bridge_aabbccddeeff`.

The integration uses the MAC-derived config-entry device ID from the beginning
of platform setup. Its device-registry identity therefore cannot change based
on which retained MQTT message arrives first. During an upgrade it removes an
empty legacy `2GIG GC2 Panel` registry device after all entities have migrated
to the MAC-identified device; a device containing any entity is never removed.
Firmware also re-sends deletion
messages for the obsolete native MQTT discovery entities whenever Home
Assistant announces that it is online.

Retained zone records make setup independent of timing. New zones and user IDs
are added while the integration is running; renamed zones update without
changing their unique IDs. A disabled zone's retained record is deleted after
the next programming poll so it is no longer treated as part of the inventory.

Zone device classes are conditioned as follows:

| GC2 meaning | Home Assistant device class |
|---|---|
| Door, entry, or gate | `door` |
| Garage door | `garage_door` |
| Window | `window` |
| Motion/interior | `motion` |
| Smoke/fire | `smoke` |
| Carbon monoxide | `carbon_monoxide` |
| Gas | `gas` |
| Water/flood | `moisture` |
| Glass break | `sound` (when supported by the panel metadata) |
| Unclassified contact | `opening` |

Home and Away arming, disarming, and bypass changes go to the actual GC2. The
integration does not optimistically change state: it waits for the console to
report what the panel really did. The ESP bridge maintains and periodically
verifies debug access so allow-listed controls do not pay the full unlock delay
for every action. An open zone continues to report open when physically
bypassed; bypass is separate GC2 state.

Alarmo is not required and should not be configured as a second independent
alarm engine for the same system. The integration uses Home Assistant's standard
alarm and binary-sensor interfaces, so ordinary dashboards and automations can
display and control the GC2 directly.

## Factory reset

After the sketch is running, hold the board's **BOOT** button for 10 seconds.
The bridge erases Wi-Fi, the Tera Term password hash, and all MQTT settings,
then restarts in provisioning mode. Do not hold BOOT while resetting or
applying power because GPIO0 is also the ESP32-S3 boot-mode strap.

## Security notes

Telnet and ordinary MQTT are plaintext protocols. Credentials and panel data
can be observed by anyone able to capture traffic on that LAN. Use a trusted,
isolated network and equipment you are authorized to service.

The provisioning network is open by design. Provision in a controlled location
because anyone within radio range can submit new settings while it is active.
The setup network is disabled after a successful connection and is never
reopened merely because the configured Wi-Fi network is unavailable. A physical
10-second BOOT-button factory reset is required before it can return.

Monitor and maintenance modes deliberately keep the GC2 debug console unlocked;
`/listen on` is the only strict zero-transmit mode and pauses those checks.
Maintenance mode additionally exposes state-changing GC2 commands to an
authenticated user. The pacing limit prevents overrunning the console but does
not make dangerous panel commands safe. Keep deployed or monitored alarm
services disconnected during bench work.
