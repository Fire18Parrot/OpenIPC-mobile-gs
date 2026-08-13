# Prototypes

Design mockups worked on before anything is committed to Kotlin. They are
standalone HTML — no build step, no dependencies, no network. Download one and
open it in any browser, including the phone's.

## `goggles-ui.html`

The ground-station menu drawn in the DJI Goggles 2 visual language, as an
alternative to the ported SBC look currently in the app.

Every row is a real item from the ported `gsmenu` tree (see
`app/src/main/kotlin/org/openipc/mobilegs/ui/gsmenu/GsMenuModel.kt`), carrying
its upstream `<target> <section> <item>` path. The point of the mockup is that
this is a **second renderer over the same data**, not a different menu: DJI's
five rail entries map almost one-to-one onto sections we already have.

| DJI rail | Our section |
| --- | --- |
| Status | live link stats — RSSI, SNR, FEC, bitrate, codec |
| Album | DVR recordings |
| Transmission | `gs wfbng` — channel, bandwidth, TX power, adaptive link |
| Settings | `gs system` — codec, DVR, video scale |
| More | APFPV, WiFi, MAVLink endpoints, gs.key, diagnostics |

### Using it

- Click a rail entry to change section.
- Click a row to open its list of values, then pick one. Switches toggle in
  place.
- Keyboard: <kbd>↑</kbd><kbd>↓</kbd> move, <kbd>Enter</kbd> opens the list,
  <kbd>Esc</kbd> closes it. <kbd>←</kbd><kbd>→</kbd> still nudge a value
  without opening anything, which is quicker on a two-option row.
- **Flight view** at the bottom previews the OSD in the same language: the
  ported `osd.json` metrics box at the top right, and the corner strip at the
  bottom right, where the goggles keep their numbers.

  The strip's readings are drawn, not spelled out, wherever a drawing reads
  faster in flight — a battery cell filled to its charge, six bars on
  `osd.json`'s own band table, one bar showing clean against FEC-recovered
  against lost packets, and a trace of throughput over the last few seconds.
  Each element is its own toggle under Settings > Camera, alongside the
  no-signal background and a switch for the top-right metrics box.

Rows greyed out are present in the SBC menu but cannot be served by a phone —
the HDMI connector, DVR re-encoding, the SBC's own Wi-Fi and audio plumbing, and
the Air section until there is an SSH bridge to the camera. They stay listed
because dropping them would make this a different menu from the one a pilot
already knows.

### Tweaking

Everything visual is a token at the top of the file:

```css
--panel: rgba(58, 62, 68, 0.78);   /* panel translucency over video */
--focus: #ffd400;                  /* the yellow selection ring */
--on:    #4cd964;                  /* toggle, and good-state values */
--rail-w: 132px;                   /* icon rail width */
--panel-w: 520px;                  /* content panel width */
--row-h: 52px;                     /* row height */
```

Menu content lives in the `SECTIONS` array in the same file, so rows can be
added or reordered without touching the layout.

### Deliberate choices, open to argument

- **Single theme.** A goggles display over live video; a light variant would be
  wrong, so every colour is painted explicitly rather than offering both.
- **Translucent panels**, as in the reference photos. Easy to make opaque if it
  proves too busy against moving video.
- **System font stack.** DJI's face is proprietary; this uses a DIN-adjacent
  stack that is close in feel and ships with the platform.
- **Two detached panels** with a gap between them, matching the references,
  rather than one merged surface.
- A few rows use DJI's phrasing where it reads better than upstream's
  (`Record With`, `Video Coding Format`).
