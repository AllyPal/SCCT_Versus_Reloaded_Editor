# SCCT Versus Reloaded Editor
SCCT Versus Reloaded Editor is an unofficial patch for the Unreal Level Editor used by Splinter Cell: Chaos Theory's Versus mode. It is compatible with the stock game as well as [Enhanced SCCT Versus](https://github.com/Joshhhuaaa/EnhancedSCCTVersus).

## Install
- Extract the contents of the archive into the game's `/System` directory, where `ChaosTheory_Editor.exe` is located.
- Run `Reloaded_Editor.exe` and check that the title bar displays **Reloaded Chaos Theory Editor** to confirm the patch is active.

## Features
### Fix Object Selection Delay
The Reloaded Editor removes the long delay that occurs when selecting objects in the editor viewports on modern hardware.
<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/fcb4655e-bc66-4337-ac5f-090eb4954dbe"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/fbb091bb-f12d-42f7-b708-629bf5f0ad0f"></td>
    </tr>
    <tr>
      <td align="center">Stock</td>
      <td align="center">Reloaded</td>
    </tr>
  </table>
</div>

### Lighting Fixes (Windows only)
Echelon lights (those using `LightEffect=LT_ESpotShadow` in the editor) do not render on modern graphics cards by default. The Reloaded Editor restores their visibility.
<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/380bcf95-f693-4018-8bfd-4dead29486dc"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/91604cd8-0008-4c06-9043-161ae26b2185"></td>
    </tr>
    <tr>
      <td align="center">Stock</td>
      <td align="center">Reloaded</td>
    </tr>
  </table>
</div>

### Increased Lightmap Resolution and Quality
The Reloaded Editor increases all selectable lightmap resolutions by 2x. The previous maximum of 256x256 has been raised to 512x512. Compression has also been disabled. In the stock editor, although the maximum resolution was 256x256, compression reduced the actual quality to something closer to 128x128.
<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/cff3cdca-656e-4028-bcf6-38f17c5fb287"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/98f707c4-3eee-4737-99d6-ef2483c4b763"></td>
    </tr>
    <tr>
      <td align="center">Stock</td>
      <td align="center">Reloaded</td>
    </tr>
  </table>
</div>

### Increase Audio File Size Limit
In the stock editor, importing WAV audio files larger than 200KB would immediately crash the editor. The Reloaded Editor removes that limitation and allows larger WAV files to be imported.

#### Importing WAV Files
1. Run `Reloaded_Editor.exe` with the `-log` argument to open the Unreal Log Window.
2. In the Unreal Log Window, run the following command to import a WAV file:
> ```
> AUDIO IMPORT FILE="C:\Path\To\YourSound.wav" PACKAGE="PackageName" NAME="SoundName" GROUP="GroupName"
> ```
> - `PACKAGE` should typically be `Amb_<mapname>` (e.g. `Amb_Museum`)
> - `GROUP` can be any organizational name (commonly `Ambiances`)
> - `NAME` is the internal name the sound will use in the engine
>
3. After importing, save the package using:
> ```
> OBJ SAVEPACKAGE PACKAGE="PackageName" FILE="..\Packages\Sounds\PackageName.usx"
> ```
> - The package must be saved with a `.usx` extension, since saving `.uax` files is blocked by the editor.
4. Manually rename the package from `.usx` to `.uax` using File Explorer.
