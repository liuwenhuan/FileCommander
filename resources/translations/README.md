# Translating File Commander

The UI is translated with Qt Linguist. Each language is a `ttc_<code>.ts` file in
this directory (`<code>` is a locale like `zh_CN`, `fr`, `pt_BR`). The compiled
`ttc_<code>.qm` files are bundled into the app via `resources/resources.qrc`.

## Add or update a translation

1. **Refresh the catalogs from the source code** (adds newly introduced strings,
   marks them unfinished):

   ```sh
   cmake --build build --target update-translations
   ```

2. **Translate** — open the `ttc_<code>.ts` for your language in **Qt Linguist**
   (`linguist`) and fill in the empty / "unfinished" entries.

   To start a brand-new language, copy an existing `.ts`, rename it to
   `ttc_<code>.ts`, and register it in `resources/resources.qrc` alongside the
   others (or just ship the compiled `.qm` — see below).

3. **Compile** the `.ts` files to `.qm`:

   ```sh
   cmake --build build --target release-translations
   ```

4. Rebuild the app so the updated `.qm` are packed into the resources, or use the
   drop-in method below.

## Drop-in a language without rebuilding

The app also loads catalogs from an **external directory** (which takes priority
over the bundled ones):

```
~/.config/totalcommander/translations/ttc_<code>.qm
```

Drop a compiled `ttc_<code>.qm` there and it appears in **View → Language** on the
next launch — no recompilation needed. This is the easiest way to test a
translation or distribute one independently.

## Notes

- Language changes apply **live** (no restart). The menus, headers, function-key
  bar, status bar, and window title retranslate immediately.
- English is the source language and has no catalog.
- Language names in the menu are shown in their own script (e.g. `简体中文`,
  `Français`) and are intentionally not themselves translated.
