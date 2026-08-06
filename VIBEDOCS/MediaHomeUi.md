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
   identity, and translucent icon-and-text search and refresh controls.
3. A landscape continue-watching rail with playback progress.
4. A landscape library rail using the server-provided library artwork.

The normal application toolbar is hidden on the trendy media home. Library,
search, settings, traditional home, and other views use the shared toolbar.

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

- The featured entry advances every ten seconds and is limited to eight dots.
- Featured backdrop images use Qt's shared image cache so the same URL can be
  reused when the carousel returns to an entry.
- The primary featured action opens the existing details flow; it does not
  bypass playback URL resolution.
- Vertical mouse-wheel input scrolls the page. Horizontal touchpad input, or
  Shift plus mouse wheel, scrolls the media rails.
- Search opens as a focused input popup from the compact toolbar button and
  closes when navigation moves to the results view.
- The combined server button returns to the source selector; search and refresh
  actions remain grouped at the top right. Refresh reloads recommendations,
  continue watching, and libraries through `AppViewModel::refreshHome`.

## Card Presentation

Continue-watching cards use 16:9 artwork with the progress bar over the bottom
edge and title metadata below. Episode metadata combines the series name and
season/episode index. Library cards use wide server artwork and an
optional item-count badge. Library and search grids use unframed portrait
posters with titles below the image, matching the home hierarchy without
nested decorative cards.

## Verification

The UI was compiled through the Qt QML cache generator and rendered against a
saved Emby session at 125% Windows display scaling. The home hero, scrolled
library rail, and library item grid were inspected using DPI-aware window
captures. Final verification must continue to include a full application build.
