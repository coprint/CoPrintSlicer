# CoPrint PEI Build Plate Assets

This profile uses a custom CoPrint PEI build plate:

- `coprint_pei_sheet_buildplate_model.stl`: physical bed shape used by the 3D scene.
- `coprint_pei_sheet_buildplate_texture.png`: runtime texture used by the slicer.
- `coprint_pei_sheet_buildplate_texture.svg`: source artwork for regenerating the PNG.

The texture SVG is based on the PEI sheet artwork with outer stroke lines disabled and logo opacity strengthened for slicer visibility.

To regenerate the PNG:

```powershell
& 'C:\Program Files\Inkscape\bin\inkscape.exe' `
  'resources\profiles\Co Print\coprint_pei_sheet_buildplate_texture.svg' `
  --export-type=png `
  --export-area-drawing `
  --export-width=4096 `
  --export-filename='resources\profiles\Co Print\coprint_pei_sheet_buildplate_texture.png'
```

After regenerating the source PNG, copy it into the build/runtime profile folder before launching the app.
