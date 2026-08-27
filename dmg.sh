#!/bin/bash
set -euo pipefail

APP_SRC="build/arm64/src/Release/CoPrintSlicer.app"
DMG_TEMP="dmg_temp"
APP_DST="${DMG_TEMP}/CoPrintSlicer.app"

# 1. Geçici klasör oluştur
rm -rf "$DMG_TEMP"
mkdir -p "$DMG_TEMP"

# 2. .app'i kopyala (geliştirme derlemesinde Resources kaynak ağacına symlink)
ditto "$APP_SRC" "$APP_DST"

# 3. Resources symlink'ini gerçek dosyalarla değiştir.
#    cp -R / ditto varsayılanı symlink'i korur; diğer Mac'te
#    /Volumes/HIKSEMI/... yolu olmadığı için uygulama açılıp hemen kapanır.
RESOURCES="${APP_DST}/Contents/Resources"
if [ -L "$RESOURCES" ]; then
  rm "$RESOURCES"
  ditto "resources" "$RESOURCES"
fi

# 4. Kaynaklar değiştiği için ad-hoc imzayı yenile (aksi halde "hasarlı uygulama")
codesign --force --deep --sign - "$APP_DST"

# 5. Applications'a symlink ekle
ln -s /Applications "$DMG_TEMP/Applications"

# 6. DMG oluştur
hdiutil create -volname "CoPrintSlicer" -srcfolder "$DMG_TEMP" -ov -format UDZO CoPrintSlicer.dmg

# 7. Temizlik
rm -rf "$DMG_TEMP"
