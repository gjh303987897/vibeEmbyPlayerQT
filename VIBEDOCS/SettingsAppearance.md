# Settings, I18n, Theme and Service Sorting

## Settings Page

The application now has a dedicated settings page for application-level options.

Current settings:

- Theme: `system`, `dark`, `light`
- Language: `system`, `zh_CN`, `en_US`
- Desktop: minimize to tray

Settings are exposed through `AppViewModel` and persisted by `SessionRepository` with `QSettings`.

QML owns only layout and user interaction. It must not write `QSettings` directly.

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

## Service Card Sorting

The final service sorting interaction is drag-and-drop.

Flow:

- QML starts a drag only when service edit mode is active.
- On drop, QML calls `AppViewModel::moveServiceCardTo(fromRow, toRow)`.
- `SessionRepository::moveServerTo` rewrites `sort_order` for visible service cards.
- `loadServiceCards` always orders by `sort_order`, then recent usage.

The previous up/down button behavior should not remain as the final UX.
