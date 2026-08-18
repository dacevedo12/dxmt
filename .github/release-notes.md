Direct3D 9 support for DXMT, built on [3Shain/dxmt](https://github.com/3Shain/dxmt).
The tarball carries the whole runtime, so it replaces an existing DXMT install
rather than adding to it: Direct3D 11, 10 and 9, plus both Wine halves, for
x86_64, 32-bit and ARM64EC guests.

## Install with CrossOver
1. Quit CrossOver.
2. From the tarball, copy the `i386-windows`, `x86_64-windows`, and `x86_64-unix` folders into `/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/dxmt/` (replacing the existing files).
3. In your game's bottle, set the graphics backend to **DXMT** and launch.

## Install with GameHub

GameHub keeps dxmt as a component, and points `WINEDLLPATH` at the `wine` folder inside it, so the three folders go one level deeper than they do under CrossOver.

1. Quit GameHub.
2. Install a dxmt graphics component from GameHub first if you have not already, so the folder below exists.
3. From the tarball, copy the `i386-windows`, `x86_64-windows`, and `x86_64-unix` folders into `~/Library/Application Support/com.gamemac.www/wine-engine/downloads/dxmt-nightly/wine/` (replacing the existing files).
4. Select **dxmt** as the graphics stack for your game and launch.

If your dxmt component is not called `dxmt-nightly`, use whichever folder under `wine-engine/downloads/` contains a `manifest.json` next to a `wine` folder. Replacing a component's contents this way leaves GameHub still reporting the version it originally installed.

## Install with Sikarugir

Sikarugir builds a self-contained app per game, each carrying its own Wine engine, so the files go inside the app you want to use them in rather than in one shared place.

1. Quit the app.
2. Right-click the generated app, choose **Show Package Contents**.
3. From the tarball, copy the `i386-windows`, `x86_64-windows`, and `x86_64-unix` folders into `Contents/Frameworks/renderer/DXMT/wine` (replacing the existing files).
4. Enable DXMT for that app and launch.

Repeat per app: an app built with its own engine has to be updated separately.

## Install with Procyon

Follow the same steps as CrossOver but in the following paths:
- `/Applications/Crossover_patched.app/Contents/SharedSupport/CrossOver/lib/dxmt/`
- `/Applications/Crossover_patched_x87/Contents/SharedSupport/CrossOver/lib/dxmt/`

## ARM64 bottles (FEX)

The `aarch64-windows` and `aarch64-unix` folders are for prefixes whose guest is
ARM64 rather than translated x86_64, such as CrossOver's ARM preview. Use them
in place of the `x86_64-*` pair, alongside `i386-windows`.

What is known about that path: the layer loads, creates devices and renders,
passing 132 functions of the Direct3D 9 visual conformance suite and 104 of the
device suite under FEX. What is not: no game has been played on it end to end.
Treat a score from it as a first data point rather than a measurement, and
expect titles that lean on x87 to behave differently than they do under Rosetta,
since there is no x87 accelerator on that path.

## A note on performance with older 32-bit games

dxmt renders correctly, but it does not accelerate x87 floating-point. Most
2000s-era 32-bit D3D9 games are x87-heavy, so under plain Rosetta you _may_ see
low fps. For full speed you can use an x87 accelerator (rosettax87). 64-bit and
non-x87-heavy titles should be fine.

## Benchmarks

Unigine Tropics, 3DMark 06 and the Devil May Cry 4 benchmark all run to
completion and report a score.

<img width="330" alt="Unigine Tropics" src="https://github.com/user-attachments/assets/58b0e521-e0c4-4d79-8ae6-28d13558169b" />
<img width="330" alt="3DMark 06" src="https://github.com/user-attachments/assets/72a691d9-714d-4511-ab22-ca54f8cb8335" />
<img width="330" alt="Devil May Cry 4 benchmark" src="https://github.com/user-attachments/assets/df5abad7-6a96-4c0f-8a17-946557843770" />

## Direct3D 9 titles

Captured while bringing the Direct3D 9 frontend up, on Apple Silicon under Wine.

<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/52605fef-9ad1-4416-ab0b-4bc7ee5c936a" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/06cb0f10-8b34-43dd-adfb-585076a5d5f3" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/2977c17c-c8bf-4062-b460-177b466857dc" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/9ab1fa8e-22d4-4111-a561-afbec6455459" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/cfcdaac4-4267-46f6-aafe-db295a168aae" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/be5c688f-8d69-4473-8f4d-8d124aa45c98" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/b45b96b2-143c-42ef-ac97-26df7dd7cfa8" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/ede27de6-3f3e-461d-b1a3-2c871b67b390" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/902dda50-18a6-4b31-a240-2bf5e506d991" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/8565b434-1f00-46c7-8bf9-8330e8aec55b" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/f00ed526-3968-4c3e-92c1-692bc30d3ebd" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/c57de302-c37b-45b6-a4ed-5a7a78b87396" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/9e77430a-2618-46d5-acf3-3be662737926" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/81fa0ad6-0a48-467e-ae88-d14ded344126" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/e9097e72-e0bc-4a24-967e-190271d79db3" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/f27f56f2-7837-4c64-89fe-8fcc57a41aed" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/5f34e779-2f6a-46d4-9830-b3669b1d19ce" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/616eac3e-230b-4403-9478-66252b289388" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/72f5e383-e097-40a5-bb57-4f5dbd114ce3" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/479ee69c-82ad-44bd-904d-3309db1823bf" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/effeb003-353b-4dbd-ac5a-610f514cd34b" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/d10655f3-9b24-4dfc-a234-2c3d34e7f261" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/005286cb-9816-41cd-86f8-4f99d0625ab8" />
<img width="330" alt="dxmt Direct3D 9" src="https://github.com/user-attachments/assets/4aea5094-c083-4318-bed4-83eaedd045e9" />
