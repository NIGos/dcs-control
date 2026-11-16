# Application icon

The original icon follows the revised launcher theme: charcoal `#262626`, warm orange `#f5a01a`, an aircraft silhouette and four focus brackets. It contains no lettering or third-party artwork. The sidebar uses the same mark without the tile; the application and favicon use its rounded charcoal tile.

Files:

- `src/DcsVr.App/Assets/app-icon.ico`: 32-bit RGBA PNG frames at 16, 20, 24, 32, 40, 48, 64, 128 and 256 px. Intermediate sizes support Windows display scaling. Tiny frames omit the canopy cutout to keep the silhouette clear.
- `src/DcsVr.App/Assets/app-icon.png`: 1024 px image with transparent corners.
- `src/DcsVr.App/Assets/app-icon.svg`: editable, scalable icon.
- `src/DcsVr.App/ui/brand-icon.svg`: matching transparent rail mark.
- `src/DcsVr.App/ui/app-icon.svg`: local favicon.

The executable embeds the ICO through ApplicationIcon. The native window loads an embedded copy for its taskbar/Alt+Tab icon. The installer assigns the executable's first icon to its Start menu shortcut. Assets are also copied into the application distribution.

Rebuild the assets with `py -3 scripts/build-app-icon.py` (requires Pillow). Both the SVG and raster exports use the same coordinate data and palette in that script. Edit that source for reproducible changes; the checked-in exports make Pillow unnecessary for a normal .NET build.

This icon was authored directly from vector geometry to match the project's existing SVG graphics. No image-generation service was used.
