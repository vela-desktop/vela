<!-- SPDX-FileCopyrightText: 2026 Brando Giuffrida -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Keyboard layout flags

The country flags that Settings > Keyboard > Layouts shows next to each
layout ([docs/keyboard.md §5.3](../../docs/keyboard.md#53-flags)), compiled
into Settings' resources as `:/flags/<country>.svg`.

- **Source:** [flag-icons](https://github.com/lipis/flag-icons) v7.6.0,
  the `flags/4x3` set, unmodified. Release tarball SHA-256:
  `e6718c0e6cb97362c33c086f8083e46d0e770d644cf046b3bbf8988408ef7250`.
- **Licence:** MIT, in [LICENSE](LICENSE) (Copyright (c) 2013 Panayiotis
  Lipiridis). It applies to the SVG files here; this README is Vela's.
- **Which:** only the countries that xkeyboard-config 2.49 names in
  `evdev.xml` and `evdev.extras.xml` (129). A layout of a country without
  a file here shows the generic keyboard icon.

The files hold no script, image or external link; check again when
updating. To update, from an unpacked release of flag-icons in `$FLAGS`:

```sh
for c in $(grep -oh '<iso3166Id>[A-Z]*' /usr/share/X11/xkb/rules/evdev.xml \
        /usr/share/X11/xkb/rules/evdev.extras.xml | cut -d'>' -f2 | tr A-Z a-z | sort -u); do
    cp "$FLAGS/flags/4x3/$c.svg" settings/flags/
done
grep -l -i -E '<script|<foreignObject|<image|on[a-z]+=' settings/flags/*.svg # must print nothing
```
