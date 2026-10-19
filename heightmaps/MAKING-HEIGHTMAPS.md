# Making height maps - for modders

The relief the renderer draws on the walls (`Geometry=POM` or `Geometry=DisplacementMapping` in
`SecondRevision.settings.ini`) comes from a height map for each texture. The maps of a texture package come in a file
beside it, so any package can have them: a mod's own, a replacement's, the original game's. A texture without a map
is drawn as it always was.

## Where the file goes

`<Package>.heightmap`, beside `<Package>.utx`, in the folder the game loads that package from. The renderer asks the
engine where it loaded the package from, so a mod that replaces a package ships its own `.heightmap` beside its own
`.utx`, made from its own pictures.

## What is in it

A zip archive, with the extension `.heightmap`, holding:

- **`<Package>.heightmap.txt`** - the list: a line for each texture, three fields separated by TABs:

      Package.Name<TAB>file in the zip<TAB>depth

  - the name the game calls the texture by: its package and its own name, **without the group**
    (`CoreTexEarth.ClenLtan Adobe_`), case does not matter;
  - the DDS file in the zip that holds its map, case does not matter;
  - how deep the relief goes, in Unreal units - a decimal point or comma; without the field it is 1; 0 switches the
    texture's relief off.

  A line without a TAB is skipped. A name may be 95 characters at most, a file name 127; the renderer holds 2048 maps
  and 512 packages in all. For example:

      CoreTexEarth.ClenLtan Adobe_	ClenLtan Adobe_.dds	1.0

- **One DDS for each texture** - one channel, block compressed: BC4 (`DXGI_FORMAT_BC4_UNORM` in a DX10 header, or the
  `ATI1` / `BC4U` four-character codes), at most 16384 pixels a side. Give it its mip chain down to 4x4: the far
  surfaces read the small copies, and the renderer takes up to 14 levels.

## The map itself

- **White (1.0) is the surface, black (0.0) the deepest point.** The relief is carved into the surface; nothing rises
  above it.
- **The same picture as the texture.** The map is read with the texture's own coordinates, so it has to cover the
  texture's area exactly, in the same aspect; its size is up to you.
- **The size.** For a high-resolution texture a quarter of its width and height (a sixteenth of the pixels) is
  enough: in our tests it kept the detail everywhere but at the closest look at the finest textures. Keep each side at
  128 or more; textures of 256 or less can keep their full size. Smaller maps load faster and take less video memory.
- **Making one.** From the texture itself, with tools like Adobe Substance 3D Sampler, or any tool that makes a height
  map out of a picture. Then look at it in the game (below): what reads as deep there is what matters.
- **No relief on some surfaces.** A mirror and a see-through surface (glass, water) get none, whatever their map.

## Packing it

An **uncompressed ("stored") zip is the best** for this. The BC4 maps are compressed already, so packing them again
saves little, while the renderer reads a stored map straight out of the file, without unpacking it first - the levels
load faster. A compressed zip works too.

With 7-Zip, from the folder that holds the maps and the list:

    7z a -tzip -mx=0 MyPackage.heightmap *.dds MyPackage.heightmap.txt

With Python: `zipfile.ZipFile("MyPackage.heightmap", "w", compression=zipfile.ZIP_STORED)`.

## Tuning the depth in the game

1. Have the relief on - the Recommended and INeverAskedForThis presets have it; under `Custom`, `Geometry=POM` or
   `Geometry=DisplacementMapping` - and set `DebugReadout=On`: the readout shows the texture under the crosshair and
   its depth.
2. Aim the crosshair at the texture.
3. Change it in the renderer's menu (its key is `DebugKey`, Scroll Lock by default): the line `THIS TEXTURE` switches
   the texture's relief on or off, `ITS DEPTH` sets how deep it goes.

   A hidden option makes it quicker: with `HeightMapWheel=On` added to `SecondRevision.settings.ini` by hand, the mouse
   wheel steps the depth of the texture under the crosshair without opening the menu - half a unit a notch, up deeper,
   down shallower, between 0 and 32. The turn still reaches the game as well - by default it steps through your belt -
   so it is worth taking the mouse wheel out of the game's own control settings while you tune.
4. Every change is written at once into a `<Package>.heightmap.txt` **beside** the `.heightmap` file - the renderer
   reads that one instead of the list in the zip. When you are done, put that list into the zip in place of the old
   one, and delete the loose file: what you release then carries your depths.
