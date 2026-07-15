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
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/b7e62711-54ec-40a1-a4f4-4a2d7a29b4ff"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/e6678ff8-3d73-42ed-9104-5e05bcd055ef"></td>
    </tr>
    <tr>
      <td align="center">Stock</td>
      <td align="center">Reloaded</td>
    </tr>
  </table>
</div>

### Lighting Fixes
Echelon lights (those using `LightEffect=LT_ESpotShadow` in the editor) do not render on modern graphics cards by default. The Reloaded Editor restores their visibility.
<div align="center">
  <table>
    <tr>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/4b2d0246-b865-4cf8-995c-d19209188e7b"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/112e32fc-0572-40c4-9761-4acbc71be45e"></td>
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
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/eafc2ff9-dc05-479a-aa8e-07718d1ba12d"></td>
      <td width="50%"><img style="width:100%" src="https://github.com/user-attachments/assets/262e1139-fd49-4ef0-85ff-d69a91699352"></td>
    </tr>
    <tr>
      <td align="center">Stock</td>
      <td align="center">Reloaded</td>
    </tr>
  </table>
</div>

### Increase Audio File Size Limit
In the stock editor, importing WAV audio files larger than 200KB would immediately crash the editor. The Reloaded Editor removes that limitation and allows larger WAV files to be imported.
