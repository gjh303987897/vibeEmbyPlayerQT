# Media Service Home UI

## Scope

The media home presentation is implemented in `qml/Main.qml` and consumes only
models and commands exposed by `AppViewModel`; it does not issue network
requests or parse server responses. Emby offers both trendy and traditional
layouts. Jellyfin offers the same two layouts through an independent setting.

## Layout

The trendy home follows a cinematic, vertically scrollable structure:

1. A full-width featured backdrop driven by up to eight recommended series
   from the active Emby or Jellyfin server, with continue-watching as the
   fallback data source.
2. A floating toolbar with an explicit return-to-services action, service
   identity, and translucent icon-and-text search and refresh controls
   (`HeroToolbarButton`: translucent white pill, radius 8, hover/press tints).
   The detail page reuses the same pills for its floating overlay actions
   (back at top-left; search and more at top-right), replacing the earlier
   solid circular back button and icon-only capsule. The detail search popup
   replays the home search expand-from-button transition (220ms grow out of
   the pill, 150ms collapse back, x/y anchored to the button via mapToItem).
3. A landscape continue-watching rail with playback progress.
4. A landscape library rail using the server-provided library artwork.

The normal application toolbar is hidden on the trendy media home. Library,
search, settings, traditional home, and other views use the shared toolbar.

Clicking a continue-watching card (trendy and traditional rails) expands the
poster itself to full window via the shared `serviceTransitionOverlay` in
poster mode (`root.openMediaCardFromCard(card.posterItem, …)`): the growing
surface is filled with the card artwork (no brand tile/halo/emblem/title),
then releases onto the detail page like a service-card transition. Poster
transitions are one-shot (`finishTransition` clears `hasSource`) so a later
“back to services” shrink never reuses a stale poster rectangle.

The traditional media home restores the standard application toolbar and page
spacing for either Emby or Jellyfin. It presents portrait continue-watching
cards in a horizontal rail and libraries in a responsive grid. Traditional
cards preserve the series-name and season/episode metadata added to the trendy
home.

## Data Contract

`AppViewModel::recommendedItems` is populated from Emby's official
`GET /Users/{UserId}/Suggestions` endpoint with `IncludeItemTypes=Series`, or
Jellyfin's official `GET /Items/Suggestions` operation with `type=Series`.
Failed or empty recommendation responses fall back to `continueItems`, so
recommendation availability never blocks the home screen.

The featured area reuses `MediaItemListModel` roles populated by the service
layer:

- `backdropImageUrl`, falling back to `continueImageUrl`
- `name` and `seriesName`
- `overview`
- `communityRating`, `productionYear`, `officialRating`, and `runTime`
- season and episode indexes
- `playedPercentage`

Recommended-series selection calls `AppViewModel::openRecommendedItem` and
uses the existing details flow. Continue-watching selection still calls
`AppViewModel::openContinueItem`, and library selection still calls
`AppViewModel::openLibrary`.

Layout selection is exposed independently by `AppViewModel::embyHomeLayout`
and `AppViewModel::jellyfinHomeLayout`. Switching a layout does not trigger
additional requests; both presentations reuse `continueItems`, `libraries`,
and the existing details/navigation commands.

## Interaction Rules

- The home page is cache-first. `AppViewModel` stamps `m_lastHomeRefreshAt` on
  every real `refreshHome()`; re-entering the home view within 2 minutes with
  the same session and populated rails (`reuseCachedHomeData()`) keeps the
  in-memory data on screen, skips the loading panel, and schedules a quiet
  `backgroundRefreshHome()` after 650 ms (one transition beat). The quiet pass
  (`m_homeCacheRefreshing`, single-shot) routes the three home fetches through
  the same network calls but with `quiet = true`: callbacks swap models in
  place, never touch `homeLoading`/spinner, and failures only log (cached
  rails stay). `invalidateHomeLoading()` (leaving home) and `openLibrary()`
  stop the warm timer and drop the flag so responses can never swap rails
  while another page or server is visible; the quiet pass also bumps
  `m_homeRequestGeneration` so its own stale callbacks self-cancel.
- The trendy/traditional home Loaders stay `active` while logged in (not only
  while visible) so returning from details/library/search is a plain reveal
  instead of an asynchronous delegate rebuild that flashed the loading panel
  over perfectly good cached data. The loading panel still covers the first
  incubation (`status !== Ready`) and the empty-models first fetch.

- The featured entry advances every ten seconds and is limited to eight dots.
- Featured backdrop images use Qt's shared image cache so the same URL can be
  reused when the carousel returns to an entry.
- The primary featured action opens the existing details flow; it does not
  bypass playback URL resolution.
- Vertical mouse-wheel input scrolls the page. Horizontal touchpad input, or
  Shift plus mouse wheel, scrolls the media rails.
- Emby omits the left and right navigation buttons from the continue-watching
  rail in both layouts. Drag, touchpad, and wheel navigation remain available.
  Jellyfin retains its explicit rail navigation buttons.
- Search opens as a focused input popup from the compact toolbar button and
  closes when navigation moves to the results view.
- The combined server button returns to the source selector; search and refresh
  actions remain grouped at the top right. Refresh reloads recommendations,
  continue watching, and libraries through `AppViewModel::refreshHome`.
- On Windows, the empty space between the server button and the right-side
  actions is a native window-move area. Dragging it moves the frameless window,
  while double-clicking toggles maximized and normal states without intercepting
  the adjacent toolbar actions.
- Trendy and traditional roots are asynchronous, mutually exclusive loaders.
  The selected tree is incubated only while the home page is visible, and a
  lightweight loading panel remains visible while that work completes.
- Loading indicators use the shared rectangle-dot spinner and render-thread
  rotation animator instead of `BusyIndicator`, avoiding a first-use control
  effect cost when a service is opened.

## Card Presentation

Continue-watching cards use 16:9 artwork with the progress bar over the bottom
edge and title metadata below. Episode metadata combines the series name and
season/episode index. Library cards use wide server artwork and an
optional item-count badge. Library and search grids use unframed portrait
posters with titles below the image, matching the home hierarchy without
nested decorative cards. All of these surfaces reuse `PosterImage`, which uses
a standalone `QtQuick.Effects.MultiEffect` to render its hidden image source
through a sibling alpha mask matching the configured corner radius. The effect
must remain separate from `PosterImage.layer.effect`: with Qt 6.7, assigning a
masked `MultiEffect` directly to that layer produces a transparent result.
Ordinary `Item.clip` remains enabled for the rectangular bounds but is not
relied upon for rounded corners. Once an image is ready, the frame background
becomes transparent and no outline is drawn, preventing a one-pixel halo at
the antialiased mask edge. Trendy home continue-watching and library artwork
use a 12-pixel mask radius so wide covers retain visibly rounded corners.

Reference: <https://doc.qt.io/qt-6/qml-qtquick-effects-multieffect.html>

## Verification

The UI was compiled through the Qt QML cache generator and rendered against a
saved Emby session at 125% Windows display scaling. The home hero, scrolled
library rail, and library item grid were inspected using DPI-aware window
captures. Final verification must continue to include a full application build.
