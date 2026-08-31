# Settings, I18n, Theme and Service Sorting

## Settings Page

The application now has a dedicated settings page for application-level options.

Current settings:

- Theme: `system`, `dark`, `light`
- Language: `system`, `zh_CN`, `en_US`
- Emby home layout: `trendy`, `traditional`; `trendy` is the default
- Jellyfin home layout: `trendy`, `traditional`; `trendy` is the default
- Player layout: `trendy`, `traditional`; `trendy` is the default
- Page transition animations: enabled or disabled, enabled by default
- Desktop: minimize to tray

Settings are exposed through `AppViewModel` and persisted by `SessionRepository` with `QSettings`.

QML owns only layout and user interaction. It must not write `QSettings` directly.

Settings, Local Playback, Link Playback, Global History, and the M3U8S manager
use the same shared header back control. The toolbar no longer carries duplicate
"Settings" and "Services" text actions alongside it; the shared back control is
the single navigation exit, and the settings entry lives in the services page
toolbar group. The traditional media home keeps its own icon-style settings
action. The shared control owns its foreground-state colors so the arrow retains
sufficient contrast against the translucent pressed background in both themes.
Its light-theme arrow uses the primary accent instead of the normal black text
color, while the button keeps a white surface with only subtle hover and pressed
tints. Theme strings are converted to typed QML colors before alpha adjustments,
preventing focused navigation buttons from rendering a black border.

## Settings Page Layout

The page is a `RowLayout` with a fixed 190px navigation panel and a scrolling
content panel. `settingsPage.categories` maps each nav entry to the ids of the
`SettingsGroup`s that make up its content, and the content Repeater re-parents
those groups out of their holder (`data: [modelData]`), so only the selected
category's sections exist in the visible tree and hidden categories cost nothing.

Selection in the nav is **one plate that slides**, not a per-row highlight. A
single `Rectangle` (`settingsNavSelection`) is declared ahead of the nav column so
it paints beneath the labels, and it tracks `settingsNavColumn.selectedItem`, the
row that last became selected. Only `y` is animated, because every row is full
width and the same height. Rows therefore animate *opacity* on hover instead of
color: a row's own background must stay transparent at rest or it would cover the
plate, and fading an opaque fill in by opacity also avoids the black-ramp problem
noted under Theme.

The content panel is deliberately **not wrapped in cards**. `SettingsGroup` keeps
its Rectangle base for layout and padding but paints no fill and no border, so
sections sit directly on the page canvas and only real controls read as surfaces.
A group shows its own heading only when its category has several sections
(`showTitle: true`, the default, used by 桌面 / 历史记录 / 隐私 under `general`);
single-section categories set `showTitle: false` because the panel heading above
already carries the same words. When editing `categories`, keep `showTitle` in
step with how many groups a category holds.

The theme row is a segmented button group rather than a combo box; see
`OptionSegmentedControl.md` for that control's sliding-thumb and even-segment rules.

## Media Server Home Layout

The Emby and Jellyfin home presentations can be changed independently without
altering media-service requests or navigation state:

- `trendy`: cinematic suggested-series hero, landscape continue-watching rail,
  and landscape library rail.
- `traditional`: standard application toolbar, portrait continue-watching rail,
  and library grid, matching the original main-branch home structure.

`AppViewModel::embyHomeLayout` and `AppViewModel::jellyfinHomeLayout` validate
their values. `SessionRepository` persists them under
`appearance/embyHomeLayout` and `appearance/jellyfinHomeLayout`. Each setting
applies only to its matching service type. Both layouts consume the same
ViewModel models and commands.

## Player Layout

The player layout can be selected independently from the media home layout:

- `trendy`: a centered, inset bottom chrome panel with a matching rounded,
  semi-transparent surface that keeps space from the window edges.
- `traditional`: a compact, semi-transparent control strip centered near the
  bottom of the video with a bottom margin, containing transport controls,
  progress, volume, tracks, speed, information, fullscreen, and exit actions.

`AppViewModel::playerLayout` validates the value and
`SessionRepository` persists it under `appearance/playerLayout`. Both layouts
use the same `PlayerPage` state and `MpvVideoItem` commands.

## Page Transitions

All primary pages share one transition treatment at the page container boundary. A page enters with a short directional slide, gentle fade, and subtle scale recovery. The direction follows the destination page order, so forward and backward navigation remain visually distinct without rebuilding persistent pages.

Entering or leaving the settings page replaces that nudge with a horizontal fly:
the slide distance grows to nearly half the container width, the fade starts from
fully transparent, and vertical offset and scale are dropped so the movement stays
strictly sideways. Settings is the last page in the stack, so opening it slides in
from the right and closing it sends the previous page back in from the left. The
distance and timing live in the `slideDistance`, `slideDuration`, `slideStartOpacity`,
`slideVertical` and `slideScale` properties of `pageStack`, which the shared
`pageEnterAnimation` reads at start-up, so both treatments come from one animation.
This is deliberately applied to the page container rather than to the settings page
itself: `StackLayout` swaps pages instantly and `Item` has no `exit` transition, so a
page cannot animate itself away.

The animation is deliberately brief and does not move navigation or persistence logic into QML. `AppViewModel::pageTransitionsEnabled` exposes the preference, and `SessionRepository` persists it under `appearance/pageTransitionsEnabled`. Disabling the option resets the page container immediately and subsequent page changes occur without animation.

## I18n

The first implementation uses a lightweight key-based translation map in `AppViewModel::trText`.

Supported languages:

- `zh_CN`
- `en_US`
- `system`

QML calls `appViewModel.trText(key)` through the local `t(key)` helper. New visible strings should use translation keys rather than hard-coded text.

Future migration to Qt `.ts` / `.qm` resources is allowed, but the ViewModel/QML boundary should remain stable.

## Theme

Theme mode is stored as:

- `system`
- `dark`
- `light`

QML maps `appViewModel.effectiveTheme` to local theme tokens. Components should use tokens such as `theme.bg`, `theme.surface`, `theme.text`, `theme.border` and `theme.primary` instead of hard-coded colors.

`theme.bg` is the page canvas (it is also the `ApplicationWindow` color), so any full-width chrome that
sits *on* the canvas - the shared `applicationToolbar` and the `windowControlsPanel` in normal mode -
uses `theme.bg` with no border. `theme.surface`/`theme.elevated` are reserved for items that must read
as raised above that canvas: cards, dialogs, popovers and inputs. Painting the header with
`theme.surface` instead made the whole 64px title band a different color from the page below it (dark
`#171c22` over `#0f1217`, light `#ffffff` over `#f5f7fb`), which read as a second window rather than as
part of the page.

Anything that still has to read as a control group inside that blended header uses one shared capsule,
published once on `windowHeader` so the groups cannot drift apart: `plateRadius` (14), `platePadding` (4),
`plateFill` = `theme.elevated` at 0.85/0.96 alpha and `plateBorder` = `theme.border` at 0.85/0.80 alpha
(dark/light). Individual components reference these instead of inventing fills.

Tints go through `root.withAlpha(token, alpha)`, which coerces with `Qt.color()` before reading channels.
`theme` stores plain strings, so `token.r` was `undefined` and `Qt.rgba()` silently painted black at that
alpha - every `withAlpha(theme.*)` site (primary focus rings, hover plates, the caption reveal strip) was
rendering grey-black: loud in the light theme, near-invisible in the dark one. Never reintroduce
`Qt.rgba(x.r, ...)` on a raw token. New strings keep going through the `AppViewModel` translation tables;
QML never hard-codes CJK literals.

A hoverable background that animates its color must rest on the **actual backing color**, never on
`"transparent"`. `ColorAnimation` interpolates ARGB channel-wise, and `"transparent"` is
`#00000000` - opaque black with zero alpha - so animating to any token starts from RGB `(0,0,0)`.
The element therefore dips *darker than the surface behind it* on the way in and again on the way
out, which reads as a blink on every hover. The settings page left navigation showed exactly this
against `theme.surface` (dark `#00000000` to `#252d36`, light `#00000000` to `#f1f5fb`, i.e. white to
near-white passing through grey). Rest it on `theme.surface`, `theme.input`, `theme.elevated` or
whatever the parent actually paints, as the rest of the app already does. A `"transparent"` resting
color is only safe with no color `Behavior` on that property, since there is then nothing to
interpolate - the player transport and filter chips rely on that.

## Service Card Sorting

The final service sorting interaction is drag-and-drop.

Flow:

- QML starts a drag only when service edit mode is active.
- On drop, QML calls `AppViewModel::moveServiceCardTo(fromRow, toRow)`.
- `SessionRepository::moveServerTo` rewrites `sort_order` for visible service cards.
- `loadServiceCards` always orders by `sort_order`, then recent usage.

The previous up/down button behavior should not remain as the final UX.
