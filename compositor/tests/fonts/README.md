# Test fonts

Subsets of Noto Sans 2.015 Regular and Italic (KDE's default font), for
`compositor.text` (`compositor/tests/text_test.c`): every machine measures the
same glyphs, whatever fonts it has. They keep the fonts' TrueType hinting and
layout tables, and render exactly as the full fonts do for these characters.
Licensed under the SIL Open Font License 1.1 ([OFL.txt](OFL.txt)); Noto has no
Reserved Font Name.

Made with fontTools 4.68 from Arch Linux's `noto-fonts` 2026.11.01:

```sh
for style in Regular Italic; do
    pyftsubset /usr/share/fonts/noto/NotoSans-$style.ttf \
        --unicodes="U+0020-007E,U+00A0-00FF,U+0192,U+01FA,U+0283,U+0300-036F,U+1EA4,U+1ED6,U+2014,U+2026" \
        --name-IDs='*' --name-legacy --name-languages='*' --hinting \
        --output-file=NotoSans-$style-subset.ttf
done
```

Basic Latin and Latin-1, the combining diacritical marks, ƒ, Ǻ, ʃ, Ấ, Ỗ, the em
dash and the ellipsis that cuts a title.
