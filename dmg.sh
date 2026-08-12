# 1. Geçici klasör oluştur
mkdir -p dmg_temp

# 2. .app'i kopyala
cp -R build/arm64/src/Release/CoPrintSlicer.app dmg_temp/

# 3. Applications'a symlink ekle
ln -s /Applications dmg_temp/Applications

# 4. DMG oluştur
hdiutil create -volname "CoPrintSlicer" -srcfolder dmg_temp -ov -format UDZO CoPrintSlicer.dmg

# 5. Temizlik
rm -rf dmg_temp