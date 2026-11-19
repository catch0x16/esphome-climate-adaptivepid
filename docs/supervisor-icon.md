# Supervisor icon

The icon combines an orange flame, a cyan snowflake, and a connected control hub
to represent coordinated heating and cooling across rooms.

The integration ships transparent PNGs in
`custom_components/climate_adaptivepid_supervisor/brand/`:

- `icon.png`: 256 × 256 pixels.
- `icon@2x.png`: 512 × 512 pixels.

[Home Assistant 2026.3 and newer load local brand images automatically](https://contributors.home-assistant.io/core/integrations/brands/).
No manifest entry is needed. Earlier versions can still run the integration,
but do not load these local brand images.

## Generation

Created with the built-in image generation tool, with transparency enabled.
The alpha channel was cleaned to retain the connected opaque symbol before
downsampling to the two delivery sizes.
Generation prompt:

> Use case: logo-brand. Create one polished compact icon for a Home Assistant custom integration named Adaptive PID Climate Supervisor, a thermal coordinator for multiple ESPHome heating and cooling controllers. Icon only, no words or letters. A single cohesive geometric mark: warm orange flame on the left and icy cyan snowflake on the right, joined through a small central circular control hub with three simple connected satellite dots beneath, communicating coordinated rooms. Strong clear silhouette, balanced symmetry of visual weight, thick rounded strokes, simple flat solid shapes, generous negative space. Professional restrained technical identity. Readable at 48 pixels. Orange and cyan only with dark navy hub/connectors that also remain visible on dark backgrounds using a subtle pale outline if necessary. No gradients, no shadows, no 3D, no container tile, no house outline, no thermometer, no decorative details. Transparent background. Square centered composition, symbol occupies about 82 percent of canvas.

Cleanup prompt, using the first icon as the edit target:

> Clean up this icon into a pristine flat vector-style raster icon. Preserve exactly the same composition, flame left in orange, half snowflake right in cyan, navy circular coordination hub with white rim and three navy satellite circles below joined by white-rimmed navy branches. Remove ALL scattered pixels, ALL wisps, ALL noise and ALL ragged white or colored patches between and around the shapes. All negative spaces must be completely transparent, especially the triangular spaces between the three lower branches. Solid uniform fills, smooth crisp antialiased edges, no gradients, no texture, no shadows. Preserve the main silhouette and colors. Transparent square background. One icon only, no text.
