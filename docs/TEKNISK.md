# VeyAmux for Linux – teknisk dokumentation

Kort introduktion og installation: se [README](../README.md).

VeyAmux er en native Linux-version bygget på **Veyra-NRVideo 2.1.5** (Windows, GPL-3.0, https://github.com/Likely7/Veyra-NRVideo):
**Qt 6/QML UI + FFmpeg + Vulkan compute**. Genbruger Veyras originale HLSL-shaders
(`shaders/veyra/*.hlsl`) ved at kompilere dem til SPIR-V med Microsofts DXC – ingen omskrivning af shaderne.

## Byg AppImage
```
sudo apt install cmake ninja-build g++ pkg-config patchelf file python3 glslang-tools qt6-base-dev qt6-declarative-dev \
  qt6-svg-dev qt6-wayland libvulkan-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  libpulse-dev libva-dev qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtquick-layouts
./scripts/build-appimage.sh          # -> dist/VeyAmux-x86_64.AppImage
```
`scripts/fetch-tools.sh` henter DXC, linuxdeploy og appimagetool automatisk. Kør AppImage uden FUSE: `APPIMAGE_EXTRACT_AND_RUN=1`.

## Versionsnumre
VeyAmux har sine egne versionsnumre: **1.0, 1.1 … 1.9, 2.0, 2.1** og så videre, ét trin for hver opdatering på GitHub.
VeyAmux udgives kun som AppImage (Flatpak-udgaven er fjernet efter 1.1).
- **Tagget** hedder `VeyAmux-V.<version>` (fx `VeyAmux-V.1.0`), både i VeyAmux-repoet og i fork'en. Udgivelsen får
  samme navn.
- **Ved hver udgivelse ændres nummeret tre steder:** `setApplicationVersion` i `src/main.cpp`, `project(... VERSION)`
  i `CMakeLists.txt` og `<releases>` i `packaging/veyamux.metainfo.xml`.
- **Ældre udgaver** hed `2.1.5-linuxNN` efter Veyra-NRVideo 2.1.5, sidst `2.1.5-linux21`.

## Hvad er med
| Område | Status |
|---|---|
| Afspilning (FFmpeg), seek, pause, volumen, drag&drop, fuldskærm | ✅ |
| Hardware-dekodning på AMD/Intel (VAAPI), automatisk fallback til software | ✅ (ikke testet på rigtig GPU endnu) |
| Lyd via PulseAudio/PipeWire (lyd er master-clock) | ✅ |
| GPU-kæde: RgbToLinear → [støjreduktion: temporal/NLM] → FXAA (AaLuma+AaFxaa, originale shaders) → **SGSR, AMD FSR 1, NVIDIA NIS eller AMD FSR 3.1** opskalering → sharpen/eksponering/kontrast/mætning → ScaleBlit (sRGB) eller LxPqOut (PQ, HDR-output) | ✅ |
| **AMD FSR 3.1-opskalering** (eksperimentel): FidelityFX FSR 3.1.6 upscaler (SDK 2.4.0-effektkode i SDK 1.1.4-rammen) på Vulkan med VeyAmux' egen sub-pixel-bevægelsesmåling | ✅ (blødere end Lanczos i test, se nedenfor) |
| **Lossless Scaling frame generation** (Fast 2× / Fast 3×) via brugerens egen lsfg-vk + Lossless Scaling fra Steam | ✅ testet af brugeren på RX 9070 XT / Bazzite |
| Vulkan-device vælges automatisk (diskret GPU først); `VEYAMUX_VK_DEVICE=<n>` tvinger valg | ✅ |
| Capture-kort via V4L2 (UVC): YUYV/UYVY/NV12/I420/RGB/MJPEG, valg af opløsning/frekvens, lavlatens-visning, lyd fra PulseAudio/PipeWire-kilde | ✅ (testet uden hardware: konvertering verificeret, enheden selv ikke) |
| DLSS/NGX, NVOF, NVIDIA VFG-DLL, Reflex, NVAPI, AMD HIP-NR (RDNA3) | ❌ Windows/NVIDIA-specifikke |
| **HDR-capture (P010)** → SDR: BT.2020/PQ → BT.2390-tonemapping med Veyras egne `HdrColor.hlsli`/`HdrToSdr.hlsli` (`shaders/LxP010.hlsl`) | ✅ (testet med syntetisk P010-signal, ikke med kortet) |
| **VRR-tilstand**: spillets egne billeder i jævn takt + vsync | ✅ (algoritmen simuleret, ikke testet på VRR-skærm) |
| HDR-videofiler (tonemapping ved filafspilning), HDR-output til skærmen, PS5/Xbox/Moonlight-streaming, undertekster | ❌ ikke portet endnu |

## Hvad spillet faktisk kører (capture)
Et capture-kort sender altid sit faste signal (fx 3840×2160 @ 60), uanset hvad konsollen/pc'en renderer. Infolinjen
viser derfor først det målte indhold og signalet bagefter, fx `≈1920×1080 @ 30 fps → 60 fps (Lossless Scaling 2×) · signal 3840×2160 NV12 @ 60.00 fps`
(`src/ContentProbe.cpp`):
- **Billedfrekvens (målt):** billeder, der faktisk er nye, per sekund. Alle billeder fra kortet sammenlignes på et glidende
  gitter (også dem, der springes over for lav forsinkelse); kortets egen støj måles og trækkes fra. Står billedet helt
  stille (pause, menu), vises den sidste værdi med "(stillestående billede)".
- **Opløsning (skøn, "≈"):** et opskaleret billede har et spejlbillede af sit frekvensspektrum omkring den oprindelige
  opløsnings grænsefrekvens, uanset skaleringsmetode. Én gang i sekundet analyseres 48 linjer (FFT) på en baggrundstråd og
  medianen af de sidste 5 vises. Testet rigtigt på 720p/900p/1080p/1440p/dynamisk 1728 → 4K (bilineær, bikubisk, Lanczos,
  med skarphed, med HUD) og 720p → 1080p. Begrænsninger: spil med temporal opskalering (FSR 2/3, DLSS, TAAU,
  checkerboard) eller kornfilter efter opskaleringen genskaber fine detaljer og læses derfor som højere opløsning; meget
  flade scener giver intet skøn. `veyamux-probe-test` i build-mappen tester begge målinger på rå luma-filer.

## Støjreduktion
Menu → *Forbedring* → *Støjreduktion*: Fra / Rumlig / Temporal / Begge og Let / Middel / Kraftig. Kører først i kæden
(på kildebilledet før FXAA, opskalering og skarphed), så *Skarphed* kan skrues op uden at gøre støjen skarp.
- **Rumlig** (`shaders/LxNlm.hlsl`): non-local means – hver pixel blandes med pixels i et 7×7-område, hvis 3×3-omgivelser
  og farve ligner den. Billede for billede, ingen spøgelser ved bevægelse.
- **Temporal** (`shaders/LxTemporalNr.hlsl`): rekursivt gennemsnit over flere billeder langs bevægelsen fra VeyAmux'
  egen bevægelsesmåling (`MotionEstimator`), Catmull-Rom-sampling, historikken klemmes til det aktuelle 3×3-områdes
  værdier og vægtes ned, hvor den afviger mere end støjen kan forklare.
- **Begge**: temporal, derefter en mildere rumlig.
Målt på 960×540 med tilføjet støj (σ = 7/255, PSNR mod det rene billede; "detalje" = hvor meget af den ægte fine
detalje der er tilbage, 1,00 = alt): støjfyldt 31,2–31,3 dB. Kraftig: Rumlig 37,1–37,4 dB, Temporal 35,0–38,1 dB,
Begge 36,9–40,9 dB, alle med detalje 0,97–1,02. En rød boks, der flytter 40 px/billede over grå baggrund, gav ingen
spor eller glorier (ingen pixel over 30/255 fra det rene billede). Hastighed på et rigtigt grafikkort er ikke målt her.

## Anti-flimmer og FSR 3.1.6 Native AA (TAA)
**Anti-flimmer** (`shaders/LxDeflicker.hlsl`, *Anti-flimmer* under FXAA i menuen, `enh/deflicker`):
- **Placering:** kører før støjfjernelsen på kildebilledet. Den deler én bevægelsesmåling pr. billede med Temporal
  (`EnhanceChain::recordMotion`).
- **Historik:** gemmer det forrige kildebillede, billedet før det (som det var) og billedet før det justeret til det
  forrige billede.
- **Flimmer:** en pixel flimrer, når lysstyrken går op og ned igen (eller ned og op) over tre billeder med mere end en
  tærskel, og billedet nu ligner billedet for to billeder siden.
- **To hypoteser vurderes:**
  - langs den målte bevægelse (stoles på efter konfidens og fart);
  - på samme sted uden bevægelse, kun hvor omgivelserne 3 px væk står stille. Flimmer narrer bevægelsesmålingen på
    selve de flimrende pixels.
- **Resultat:** kun dér bliver resultatet 1-2-1-gennemsnittet af de tre billeder, så tænd/sluk-flimmer bliver til sin
  rolige midterværdi. Det klemmes til nabolagets 3×3-spænd ± halvdelen af flimmeret. Ingen ekstra forsinkelse.
- **Syntetisk test:** statiske 1 px-linjer, der skifter mellem lys og mørk hvert billede:
  - flimmer 190 → 6,7 gennemsnitlig ændring pr. billede;
  - en kasse, der bevæger sig 4 px pr. billede, og den stille baggrund er urørte;
  - en panorering på 3 px pr. billede uden flimmer er urørt (fejl 0,000).
- **Tærskel:** `VEYAMUX_DF_THRESHOLD` (standard 0,02 i perceptuelt domæne).

**FSR 3.1.6 Native AA (TAA):**
- **Valg:** *Native* + *FSR 3.1* kører FSR 3.1-opskaleringen ved 1,0× (`EnhanceSettings::fsrNativeAa`).
- **Fejl:** kan FSR ikke starte, vises billedet uændret.
- **Samme test:** flimmer 190 → 59, med let blødgøring (baggrundsfejl 0,56).

## Bevægelsesmåling (FSR-opskalering)
Video har ingen bevægelsesvektorer fra en spilmotor, så VeyAmux måler dem selv til FSR 3.1-opskaleringen og den temporale støjreduktion
(`src/MotionEstimator.cpp`): luma-pyramide med op til 7 niveauer, regulariseret Lucas-Kanade fra groft til fint,
udfyldning af flade områder og til sidst skarpe bevægelseskanter (`LxMvSelect`). Over 2560 px bredde måles i halv opløsning.

## Gentagne billeder (30 fps-spil i 60 Hz-signal)
Når frame generation (Lossless Scaling) er slået til, sorteres capture-billeder, der er en gentagelse af det
forrige, fra *før* behandlingen (`FrameDiff` i `src/ContentProbe.cpp`; kortstøj måles og trækkes fra, og en gentagelse
sammenlignes med det sidst viste billede, så langsomme overtoninger stadig kommer igennem). Frame generation laver så
mellembilleder mellem to forskellige billeder: et 30 fps-spil bliver til jævne 60 fps i stedet for 60 fps med dobbelte
billeder.

Lossless Scaling: lsfg-vk skriver sin log (niveau debug) til `~/.config/VeyAmux/lsfg-vk.log`; VeyAmux viser i menuen
(og i terminalen), om laget blev indlæst, og hvilken fejl lsfg-vk eventuelt melder. *Ydelsestilstand* slår lsfg-vk's
performance mode til; flow scale vælges automatisk efter vinduets opløsning (4K 50 %, 1440p 75 %, ellers 100 %, som
Lossless Scaling anbefaler; valgt efter skærmen, så den ikke skifter – hvert skift får lsfg-vk til at bygge alt forfra).
Begge dele genindlæses af lsfg-vk, mens den kører. lsfg-vk 2 har ingen frame pacing (eneste tilstand er "vsync":
billeder vises, så snart de er klar), så resultatet er kun jævnt, når skærmens Hz er et helt multiplum af den nye
billedfrekvens (fx 60 fps × 2 = 120 fps: sæt skærmen til 120 Hz).

## OBS (browserkilde)
Menu → *OBS (browserkilde)*: slå til, vælg opløsning (720p/1080p/1440p/original) og tryk **Gem browserfil…** for at
gemme en HTML-fil, hvor du vil. I OBS: *Kilder → + → Browser → Lokal fil →* vælg filen, og sæt bredde/højde til dit lærred.
VeyAmux kører en lille HTTP-server, der kun lytter på `127.0.0.1:47920` (indstillingen `obs/port`), og sender det færdige
billede som MJPEG – efter alle forbedringer, i samme tempo som på skærmen (Lossless Scaling's mellembilleder laves
først bagefter i grafikdriverens lag og kommer ikke med i OBS) (`src/ObsStream.cpp`). I den direkte capture-vej skaleres billedet ned og kopieres fra GPU'en (`LxDownscale`,
afsluttet via en Vulkan-event, så det ikke venter på næste skærmopdatering); det sker kun, mens OBS faktisk er tilsluttet.
Siden er gennemsigtig, mens VeyAmux er lukket, og forbinder selv igen. Kun billedet sendes: lyd tages med OBS'
*Application Audio Capture* (VeyAmux) eller direkte fra capture-kortet. Adressen `http://127.0.0.1:47920/` virker også
som URL-browserkilde, og `/snapshot.jpg` giver det seneste billede.

## Opskalering
Menu → *Forbedring* → *Opskalering* (1,5× / 2× / 3×) og *Metode*:
- **SGSR** (standard) – Snapdragon Game Super Resolution 1 (Qualcomm, BSD-3-Clause, `third_party/sgsr`, uændret
  `sgsr1_mobile.h`) som compute shader (`shaders/LxSgsr.hlsl`), ét pass billede for billede, varianten med kantretning.
  Bilineær base + en Lanczos-2-formet korrektion langs kanter ud fra 12 nabopunkter (Gather på den grønne kanal), klemt
  til de 4 nærmeste punkter. Køres på √(lineært lys) og kvadreres tilbage; SGSR's `saturate` er erstattet med `max(·,0)`,
  så HDR over 1 overlever. Målt 2× (800×450 → 1600×900 af et skærmbillede, PSNR mod originalen): SGSR 26,65 dB, den
  tidligere Lanczos-3 26,53 dB, FSR 1 26,76 dB, NIS 25,46 dB – mindre ringning end Lanczos. Bruges også, når FSR 3.1
  ikke kan starte. Afløste den tidligere Lanczos-3.
- **AMD FSR 1** – EASU (kantadaptiv opskalering) + RCAS (skarphed, 0,5 stop; `VEYAMUX_FSR1_RCAS=<stop>`) fra FidelityFX
  SDK'ens `ffx_fsr1.h`, billede for billede (`shaders/LxFsr1.hlsl`), i gamma-2-domænet. Målt 2×: fine detaljer 32,2 dB /
  detaljegrad 0,93 (Lanczos 32,0 dB / 0,61), naturligt motiv 49,0 / 49,5 dB, vektorgrafik 35,6 / 37,3 dB – synligt
  skarpere uden ringning, men lidt lavere PSNR på skarpe kanter.
- **NVIDIA Image Scaling (NIS)** – NVScaler fra NVIDIAs NIS SDK (MIT, `third_party/nis`), `shaders/LxNis.hlsl`:
  6-taps retningsbestemt skalering med adaptiv skarphed, blok 32×24, 256 tråde, `NIS_HDR_MODE` lineær (kæden kører i
  lineært lys, så også HDR-signaler skaleres korrekt). Koefficient-teksturerne (2×64 RGBA32F) uploades én gang.
  Skarphed 0,4 som standard (`VEYAMUX_NIS_SHARP=0..1`). Virker på alle grafikkort (ren compute), ingen ekstra forsinkelse.
- **AMD FSR 3.1** (eksperimentel) – AMD FidelityFX FSR 3.1.6 (SDK 2.4.0-effektkode i SDK 1.1.4-rammen, open source). FSR 3.1 er en *temporal* opskalerer: den samler
  detaljer over flere billeder og skal vide, hvordan hver pixel har bevæget sig, ned til brøkdele af en pixel. Spil giver
  den præcise bevægelsesvektorer og rykker kameraet en brøkdel af en pixel hvert billede (jitter); video har ingen af delene.
  VeyAmux måler derfor bevægelsen selv (luma-pyramide + regulariseret Lucas-Kanade fra groft til fint, med udfyldning af flade
  områder; `shaders/LxLuma.hlsl`, `LxLkLevel.hlsl`, `LxMvFill.hlsl`) og kører AMD's RCAS-skarphed bagefter.
  FidelityFX optical flow (som frame generation bruger) blev forsøgt først, men giver kun hele pixels og ingen bevægelse de
  første 6 billeder efter hver nulstilling.
  **Målt resultat** (640×360 → 1280×720, PSNR mod de ægte 1280×720-billeder, panorering med skiftende sub-pixel-forskydning,
  software-GPU): naturligt motiv Lanczos 37,3 dB / FSR 3.1 30,0 dB (33,5 dB selv med perfekt kendt bevægelse); blødt motiv
  36,4 / 32,8 dB; meget detaljeret motiv 26,6 / 25,6 dB (27,2 dB med perfekt bevægelse – det eneste tilfælde, hvor FSR slår
  Lanczos). Konklusion: på video er FSR 3.1 blødere end Lanczos; derfor var Lanczos standard (nu SGSR). FSR 4.1.2 findes kun som lukkede
  Windows-DLL'er og kan ikke bruges i et Linux-program.

## Temaer
`qml/Theme.qml` styrer alle farver, hjørneradier og skrifttyper. Temaet vælges på startskærmen (`ui/theme`), skiftes med
det samme og gælder både startskærmen og menuen.

| Tema | Farver | Form og skrift |
|---|---|---|
| Vice | det oprindelige: violet med pink→orange | runde hjørner, systemets skrift |
| Code | sort med terminalgrøn (#00FF41) | mindre runde hjørner, monospace overalt |
| Inferno | næsten sort med blodrød og glød | Cinzel (OFL) til overskrifter |
| Retro | 8-bit konsol: grå, sort og rød | firkantet, 2 px sort kant med lys top og mørk bund, Press Start 2P (OFL) til overskrifter og hovedknapper, store bogstaver i knapper |

Logoet findes i én udgave pr. tema (`qml/icons/logo-<tema>.svg`, i Retro som 16×16 pixelgrafik). Vinduesikonet følger
også temaet på X11/XWayland. På Wayland bruger skrivebordet skrivebordsfilens ikon, som er Vice-logoet.

## Retro TV (CRT-Geom)
`shaders/LxCrtGeom.hlsl` er crt-geom fra ShaderGlass' forudindstilling `crt/crt-geom`. ShaderGlass har den kun som
oversat DirectX-bytekode, så det er en oversættelse af den kilde, ShaderGlass er bygget fra: libretro slang-shaders
`a4f3aee`, `crt/shaders/crt-geom.slang` (GPL-2.0-or-later). Alle parametre er ShaderGlass' standardværdier:
- CRTgamma 2,4, monitorgamma 2,2, afstand 1,5, krumningsradius 2, hjørner 0,03.
- Maske 1 (aperture grille) med styrke 0,3 og scanline-vægt 0,3.
- Interlace-simulering til.

Kørsel:
- Den sidder til sidst i kæden og tegner kilden direkte i udgangsstørrelsen, så den erstatter opskaleringsmetoden.
  Skarphed springes over, mens eksponering, kontrast og mætning stadig gælder.
- Den virker uafhængigt af Forbedring-kontakten, ligesom frame generation.

Forskelle fra originalen (billedet er det samme):
- **Compute-shader:** én tråd pr. udgangspixel. Vertex-værdierne udregnes pr. pixel, og `fwidth` udregnes ud fra
  nabopixlen.
- **Lineært lys:** kæden arbejder i lineært lys, så kilden gamma-kodes (2,2) før crt-geom's CRTgamma, og resultatet skrives
  tilbage som lineært lys.
- **Billedlinjer:** crt-geom's `ysize` er 240, 480 eller kildens egne. Et capture-kort leverer 240p skaleret op til 1080p,
  så hver simuleret linje tager gennemsnittet af de kilderækker, den dækker (op til 8), i stedet for at plukke én.
- **Maske:** kun maske 1 er med.

## Demo-scene (Abyssal Garden)
Kilden `demo:<fps>[:<højde>]` (`demo:60`, `demo:30:720`) er **Abyssal Garden** (https://github.com/Noniv/abyssal-garden, MIT, © 2026 Maksymilian
Dendura, commit `a596354`), oversat fra JavaScript/WebGPU til C++ i `src/abyssal/`.

**Shaders og grafik**
- WGSL-shaderne bruges uændret. `tools/abyssal/extract.mjs` skriver dem ud for det høje detaljeniveau, så de ligger i
  `src/abyssal/wgsl/`.
- De køres gennem **wgpu-native** v29 (WebGPU på Vulkan, MIT/Apache-2.0, linket statisk). Den hentes af
  `scripts/fetch-tools.sh`.

**Verdensgeneratoren**
- `world.js`, `math.js` og genereringsdelen af `main.js` er oversat sætning for sætning. JavaScripts regneregler er
  bevaret: doubles, float-lagring, Math.round og den samme tilfældige talrække. Hvor JavaScript trækker flere
  tilfældige tal i ét udtryk, trækkes de i samme rækkefølge.
- Resultatet er **byte for byte identisk** med originalen: terræn, cirka 2 mio. blade, sten, svampe, koraller, fisk,
  bobler, lysvolumen, kollisionsfelt og alle 26 meshes.
  - Tjek: `ABYSSAL_DUMP=<mappe> veyamux-abyssal-test <ud>` skriver C++-udgaven.
  - `node tools/abyssal/jsgen.mjs <abyssal-garden>/source/src <mappe> <js-mappe>` kører originalen på det samme
    tæthedsgitter.
  - Sammenlign de to mapper med `cmp`.

**Billede og styring**
- **Opløsning:** 1920×1080 eller 1280×720, valgt på startskærmen (høj kvalitet: 4× MSAA, 4096-skyggekort, 34
  lysstråle-trin). Dynamisk opløsning er fjernet, så VeyAmux får en fast størrelse pr. stream. Skift mellem 720p og
  1080p bygger ikke verden igen: `Scene::resize` frigiver kun de størrelsesafhængige ressourcer (alt oprettet efter
  mærkerne i `targets()`) og laver render-targets på ny. Mens størrelsen skifter, afleverer `readLatest` ingen billeder
  af den gamle størrelse. `veyamux-abyssal-test <mappe> 1280 720 1920 1080` tester skiftet.
- **Billedet til VeyAmux:** det læses tilbage som RGBA og afleveres som et capture-billede (`DemoSource.cpp`), så hele
  kæden virker.
- **Billedhastighed:** 30 eller 60 fps, holdt af rendertråden.
- **Verden bygges én gang** pr. kørsel i baggrunden, og der vises et ventebillede med fremskridt imens.
- **Styring:** taster, træk med musen og controller samles i `demo::setKey` / `addLook` / `setPad`.
  - Uden menu: venstre stik svømmer, højre stik kigger, triggere og skulderknapper giver ned/op, og venstre
    stik-klik er hurtigere.
  - Kun når menuen er lukket. Når den åbnes, nulstilles alt.

**Lyd**
- `audio.js`-grafen er bygget om til en lille synthesizer (`AbyssalAudio.cpp`). Den kører i capture-lydtråden som
  kilden `@demo`, så lydmåler, optagelse og stream får den.
- Konvolutions-efterklangen på 5 s er erstattet af en Freeverb-agtig efterklang med samme hendøen.

## Skalering til vinduet (LxFit)
Kædens resultat er kildens størrelse × den valgte faktor. Før strakte Qt det til vinduet med én bilineær sample: blødt
ved forstørrelse, og ved formindskelse (2× af 1080p i et 1440p-vindue) sprang den pixels over, så tynde linjer og tekst
flimrede. Nu fortæller `VideoItem` præcis hvor mange skærmpixels billedet dækker (`LiveVulkan::setDisplayFit`, med
devicePixelRatio), og kæden laver et sidste pass `shaders/LxFit.hlsl` til den størrelse; Qt tegner det så 1:1 (Nearest,
pixeljusteret). Samme fremgangsmåde som Veyra-NRVideo 2.2.0's "精细送显缩放": Catmull-Rom; ved forstørrelse 4×4 og klemt
til de 2×2 nærmeste (ingen ringning), ved formindskelse bredes kernen med formindskelsesfaktoren pr. akse (højst 4:1).
Kører på de kodede værdier (sRGB, eller PQ ved HDR-output). `out8_` får vinduets størrelse og `pre8_` holder billedet før
LxFit; FSR frame generation, optagelse, stream og skærmbilleder bruger det tilpassede billede. Ikke ved pixel-perfekt
(heltalsforstørrelse, skarpt). `VEYAMUX_NO_FIT=1` giver den gamle bilineære strækning.

## Pixel-perfekt skalering
Under Retro TV. To dele:
- **Visning** (`VideoItem`): billedet forstørres med et helt tal `k = floor(min(vinduesbredde/b, vindueshøjde/h))` med
  nearest-neighbour, centreret og lagt på hele skærmpixels. Uden det tegnes det med lineær filtrering.
- **Spilpixels** (`shaders/LxPixelGrid.hlsl`): finder spillets eget gitter i signalet. Hver gitterpixel er gennemsnittet
  af den inderste halvdel af de kildepixels, den dækker (`max(skala·0,25; 0,5)` til hver side), så de bløde kanter, en
  opskalerer efterlader mellem spilpixels, ikke smitter af.
  - *240p* og *480p* er faste gitre. *Auto* bruger spillets målte opløsning (samme måling som infolinjen) og skifter
    først, når den har været stabil i 1,5 s.
  - Skarphed springes over, mens pixel-perfekt er aktiv.

## Controller
`src/Gamepad.cpp` læser controllere direkte fra kernen (evdev, `/dev/input/event*`). Xbox-, PlayStation-, Switch- og
HID-driverne melder de samme standardkoder:
- `BTN_SOUTH` = A/kryds og `BTN_EAST` = B/cirkel.
- D-pad som `BTN_DPAD_*` eller `ABS_HAT0X/Y`, venstre stik som `ABS_X/Y` (tærskel 60 %).

Menuen styres sådan:
- **Åbn og luk:** Select + Start, eller Guide holdt i 500 ms.
- **Spillet får intet**, mens menuen er åben: alle controllere grabbes med `EVIOCGRAB`. Grab'et starter først, når de
  knapper, der åbnede menuen, er sluppet, og slippes først, når lukkeknapperne er sluppet, så spillet aldrig ser en
  knap, der hænger.
- **Retninger gentages:** første gang efter 380 ms, derefter hver 110 ms.
- **Navigation i QML** (`padNav` i `Main.qml`): alle synlige `MouseArea`s og skydere i panelet samles. Den nærmeste i den
  valgte retning vælges efter `afstand + 2,5 · sideafvigelse`.
  - LB/RB springer mellem kolonnerne, og venstre/højre flytter en skyder 1/40 af dens område.
  - Musebevægelse skjuler markeringen igen.

Test uden controller: `VEYAMUX_TEST_PAD_FIFO=<fifo>` læser rå `input_event`-strukturer fra en pipe.

## Frame generation
Menu → *Frame generation*:
- **Lossless Scaling – Fast 2× (fordobler, standard) / Fast 3× (tredobler)** – kræver at du har købt Lossless Scaling på Steam og har installeret
  [lsfg-vk](https://lsfg-vk.dev) 2.0+. VeyAmux finder selv Lossless Scaling i alle dine Steam-biblioteker ud fra `$HOME`
  (fx `/var/home/<bruger>/.local/share/Steam/steamapps/common/Lossless Scaling/` på Bazzite, også Flatpak-Steam og ekstra
  biblioteker fra `libraryfolders.vdf`), skriver sin egen profil til `~/.config/VeyAmux/lsfg-vk.toml` og starter lsfg-vk for
  sit eget vindue via `LSFGVK_CONFIG`/`LSFGVK_PROFILE`. Din egen lsfg-vk-konfiguration røres ikke.
  - lsfg-vk 2 bruger `lsfg-vk.dll` (fra Lossless Scalings *lsfg-vk*-gren i Steam, fanen Betaer), ikke `Lossless.dll`.
  - **Fast 3×**: tre gange billedfrekvensen (60 → 180 fps). Kræver en skærm med mindst den frekvens.
  - Slå til kræver genstart (knap i menuen), fordi laget indlæses ved opstart. **Slå fra** (Off, eller skift til FSR)
    genstarter VeyAmux af sig selv med samme kilde (`--capture last` eller filen) og fuldskærm, uden lsfg-vk
    (`PlayerBridge::restartWithSource`): lsfg-vk 2 kan ikke slås fra, mens det kører – `multiplier = 1` afvises
    ("multiplier must be greater than 1"), og ved en ugyldig fil beholder laget den gamle; uden en matchende profil
    fjernes vinduets swapchain-kontekst, og præsentationen fejler. 2× ↔ 3× hot-reloades. Af samme grund holder
    Lossless Scaling ikke pause ved 120 Hz-kilder (kun FSR gør); en besked siger det én gang. Crasher VeyAmux under
    opstart med lsfg-vk, slås Lossless Scaling automatisk fra, indtil du trykker *Prøv igen*.
  - VeyAmux indeholder/kopierer ingen lsfg-vk- eller Lossless Scaling-kode (lsfg-vk er CC BY-NC-ND 4.0) og bundler dem ikke.
  - Findes kun en ældre lsfg-vk (manifest uden `LSFGVK`, dvs. 1.x), henter VeyAmux selv lsfg-vk 2 på brugerens maskine
    (`src/LsfgInstall.cpp`): `https://builds.lsfg-vk.dev/lsfg-vk-2.0.0.tar.xz` via libcurl (kun https), tjekkes mod en
    fast SHA-256, alle arkivposter skal ligge under `bin/`, `lib/` eller `share/`, og pakkes ud med
    `tar -xJf … -C ~/.local --no-same-owner --no-overwrite-dir` (som lsfg-vk's egen vejledning). Derefter flyttes
    lsfg-vk 1's manifest og det bibliotek, det peger på (og `~/.local/lib/liblsfg-vk.so`), til
    `~/.local/share/VeyAmux/lsfg-vk-1-backup/<tid>/` – undtagen filer, lsfg-vk 2 lige har overskrevet eller selv bruger.
    En lsfg-vk 1 uden for hjemmemappen (systempakke) røres ikke; så installeres intet, og menuen siger det. Én
    installation ad gangen (`QLockFile` i `~/.local/share/VeyAmux`). Sker automatisk én gang pr. start, 3 s efter
    opstart (efter en evt. genstart med xcb); `VEYAMUX_LSFG_AUTOINSTALL=0` slår det fra, og knappen *Installér lsfg-vk 2*
    findes i menuen. Laget indlæses ved opstart, så VeyAmux skal genstartes bagefter.

## HDR-capture (P010)
Live Gamer Ultra 2.1 (og andre kort) kan levere **P010**: 10-bit Y'CbCr 4:2:0 med HDR (BT.2020-farver, PQ/SMPTE ST 2084).
Linux' uvcvideo kender formatet; i VeyAmux' capture-dialog hedder tilstandene "P010 (HDR)". P010 vælges aldrig
automatisk (dobbelt USB-båndbredde; 4K60 P010 er ca. 12 Gbit/s og passer ikke i kortets USB 3.2 Gen 2 – vælg fx 4K30
eller 1440p60, alt efter hvad kortet tilbyder). De 16-bit planer uploades som R16/RG16 og `shaders/LxP010.hlsl` regner
direkte til VeyAmux' lineære arbejdsbillede (ingen 8-bit-mellemtrin): BT.2020 NCL → PQ-afkodning → BT.2390 EETF
(Hermite-skulder i PQ-domænet) og nuance-bevarende gamut-komprimering til BT.709, SDR-hvid = 203 nits (BT.2408) – Veyras
egne HDR-funktioner. *HDR-capture: kildens maks. lysstyrke* (600/1000/2000/4000 nits, standard 1000) skal passe til
spillets/konsollens HDR-kalibrering. Testet med et syntetisk P010-signal (`VEYAMUX_TEST_LIVE=p010`: PQ-gråskala
0–4000 nits, 203-nits farvefelter i BT.2020) i den direkte capture-vej uden Vulkan-valideringsfejl; ikke testet med selve
kortet.

**HDR-videofiler** (HDR10/PQ og HLG, BT.2020): swscale leverer RGBA64 (16 bit pr. kanal) og `shaders/LxHdrRgb.hlsl` laver
samme tonemapping som capture-vejen (HLG: invers OETF + OOTF med γ = 1,2 ved 1000 nits).

## HDR-output (eksperimentel)
Menu → *HDR-output* (kræver genstart). VeyAmux beder Qt Quick om en **HDR10-swapchain** (standard: `QSG_RHI_HDR=hdr10`,
A2B10G10R10 + `VK_COLOR_SPACE_HDR10_ST2084_EXT`) eller en **scRGB-swapchain** (`scrgb`, FP16 +
`VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT`), med instans-udvidelsen `VK_EXT_swapchain_colorspace`
(`VEYAMUX_HDR_MODE=hdr10|scrgb`). Efter første billede
spørger VeyAmux selv overfladen om formatet (samme test som Qt) og slår kun HDR-vejen til, hvis det findes.
- HDR-kilder tonemappes til *skærmens top-lysstyrke* (400/600/1000/1500 nits) i stedet for 203 nits; SDR-kilder vises med
  hvid = 203 nits (BT.2408).
- Det færdige billede (`out8_`) gemmes PQ-kodet (8 bit med trekantet dither, `shaders/LxPqOut.hlsl`). I QML afkoder et
  `layer.effect` på video-elementet PQ → udvidet sRGB-kodning (`shaders/qt/pq_video.frag`), og hele vinduet tegnes i et
  FP16-lag, der omregnes til lineært scRGB (1,0 = 80 nits, SDR-hvid 203 nits) (`shaders/qt/hdr_window.frag`), så menuen ser
  normal ud.
- **Fuld BT.2020-farve**: HDR-kilder tonemappes kun i luminans (BT.2390 til skærmens top) og bliver i BT.2020
  (`HdrToDisplay2020` i `shaders/PqCode.hlsli`); hele kæden arbejder så i lineært BT.2020. SDR-kilder løftes fra
  BT.709 til BT.2020 i `LxPqOut`. `out8_` er altid PQ-kodet BT.2020 i HDR-tilstand. Video-laget dekoder til BT.709
  med fortegn (brede farver = negative komponenter), vindueslaget koder til PQ/BT.2020 (HDR10) eller lineært scRGB.
- OBS, optagelse og skærmbilleder omregnes tilbage til SDR (`PqCodedToSdr`: BT.2020 → BT.709, blød skulder på
  luminansen over 0,75, Veyras `ToneGamut`; CPU-versionen i `PlayerBridge::sdrCopy`). Målt mod SDR-vejen: højst ±15 i
  8-bit-værdi på mættede farvefelter.
- Kræver native Wayland med HDR slået til (KDE Plasma 6 / GNOME 48+, Mesa med Wayland-farvestyring). Gennem XWayland
  findes ingen HDR-swapchain; menuen siger det. Ukendt sammenspil med lsfg-vk (frame generation) på en HDR-swapchain.
- Testet her kun i en software-GPU med en SDR-swapchain (`VEYAMUX_FORCE_HDR_OUT=1`): kæden, lagene og SDR-kopierne virker,
  ingen Vulkan-valideringsfejl. Ikke testet på en ægte HDR-skærm.

## gamescope og Steam
- *Start i gamescope* (kun med `VEYAMUX_GAMESCOPE=1`, eksperimentel – på Bazzite afviste gamescopes WSI-lag
  VeyAmux' swapchain, og OK i fejldialogen kunne fryse skrivebordet; HDR og VRR fås i stedet direkte på Wayland): VeyAmux genstarter sig selv som
  `gamescope -f -W <skærm> -H <skærm> -r <Hz> --adaptive-sync [--hdr-enabled] -- VeyAmux` med `QT_QPA_PLATFORM=xcb`,
  uden `WAYLAND_DISPLAY` og med `ENABLE_GAMESCOPE_WSI=1`. Ikke inde i gamescope i forvejen (Game Mode).
  `VEYAMUX_GAMESCOPE=0` springer det over. Andre swapchain-lag ødelægger gamescopes WSI-lag ("Creating swapchain for
  non-Gamescope swapchain", kan fryse skrivebordet): inde i gamescope fjernes `ENABLE_HDR_WSI`, og lsfg-vk slås fra
  (`DISABLE_LSFGVK=1`). Crash-vagt: kommer gamescope-starten ikke op (mærket `ui/gamescopeLaunching` ryddes efter 5 s),
  slås gamescope-start fra ved næste start.
- *Tilføj til Steam*: skriver en non-Steam-genvej i `userdata/<konto>/config/shortcuts.vdf` (binær KeyValues; eksisterende
  genveje bevares, en tidligere VeyAmux-genvej opdateres, første gang gemmes `shortcuts.vdf.veyamux-backup`). Steam skal
  være lukket (den gemmer filen ved lukning), så: kører Steam, sender VeyAmux `steam -shutdown` (Flatpak-Steam: `flatpak run com.valvesoftware.Steam
  -shutdown`, valgt når den kører eller er den eneste), venter op til 45 s på, at `steam`-processen er væk, plus 1,5 s,
  skriver genvejen og starter Steam igen (detached, uden VeyAmux' egne miljøvariabler: `QT_*`, `QSG_*`, `LSFGVK_*`,
  `DISABLE_LSFGVK`, AppImage-variablerne m.fl.). Er VeyAmux selv startet af Steam (`SteamGameId`/`SteamAppId` eller
  Game Mode), lukkes Steam ikke – det ville lukke VeyAmux – og menuen beder om at starte VeyAmux fra skrivebordet.
  Biblioteksbilleder tegnes ved kørsel ud fra logoet (ingen billedfiler i pakken) og gemmes i
  `userdata/<konto>/config/grid/<appid>{p,,_hero,_logo}.png` (600×900, 920×430, 1920×620, 1280×400 gennemsigtig);
  har brugeren selv valgt et billede som .jpg/.webp, bevares det. Startindstillinger: `--fullscreen --capture last`.
  Inde i gamescope starter VeyAmux altid i fuldskærm.

## Signalskift og 120 Hz
- Fejler capture-kortet (frakoblet), forsøger VeyAmux at åbne det igen hvert sekund med samme tilstand. Kommer der intet
  billede i 3 s, genstartes streamen (hvert 3. sekund) – mange UVC-kort går i stå, når konsollen skifter
  opløsning/Hz/HDR.
- Kører capture med ≥ 95 fps (tilstandens fps eller målt indhold) i 1 s, sættes frame generation på pause (lsfg-vk-
  multiplikator 1, indstillingen bevares); under 80 fps i 2 s starter den igen.

## Lyd
Capture-lyden går igennem i blokke på 5 ms (før 20 ms) med ca. 20 ms kø ud (før 50 ms); pipewire-pulse sænker sin
kvantum tilsvarende. `VEYAMUX_AUDIO_MS=<ms>` (2–40) giver større blokke på travle systemer.

## PlayStation Remote Play (chiaki-ng)
Kilde `ps:<konsol-id>` + lydkilde `@ps` (`src/RemotePlay.{h,cpp}`, Qt-delen i `src/PlayerBridgeRemotePlay.cpp`,
vinduet `qml/RemotePlayDialog.qml`). Protokollen er chiaki-ng v1.10.1's `lib/` (AGPL-3.0, `third_party/chiaki-ng`,
bygget af `cmake/chiaki.cmake` med system-OpenSSL, libcurl, json-c, miniupnpc, libevent og Opus).
- **Find:** chiakis discovery-service (UDP-broadcast hvert halve sekund på alle netkort) mens vinduet er åbent.
- **PSN-konto-ID:** Sonys login-side åbnes i browseren; brugeren indsætter adressen med `code=`. VeyAmux henter token og
  `user_id` (libcurl) og gemmer de 8 byte little endian som base64 i `ps/accountId`. Et konto-ID kan også indsættes direkte.
- **Registrering:** `chiaki_regist` med PIN-koden fra konsollen; `rp_regist_key` og `rp_key` (morning) gemmes hex i
  `ps/consoles/<id>/` sammen med navn, adresse og target. Login-adgangskoden (4 cifre) gemmes der også.
- **Vækning:** `chiaki_discovery_wakeup` med regist-nøglen som hex-tal, gentaget hvert 5. sekund, op til 40 s, indtil
  konsollen svarer "ready".
- **Stream:** PS5 H.265 (HDR: H.265 10-bit → P010, som HDR-capture), PS4 H.264, 720p/1080p, 30/60 fps. Afkodes med
  FFmpeg + **VA-API** på grafikkortet (`LOW_DELAY`, `extra_hw_frames` 2; billedet hentes som NV12/P010 med
  `av_hwframe_transfer_data` og rækkerne kopieres), ellers software med slice-tråde (U/V flettes på CPU'en). Kan VA-API
  ikke aflevere et billede, skiftes til software fra næste nøglebillede. `VEYAMUX_PS_SWDECODE=1` tvinger software.
  F3-overlayet viser hvilken. Billedbufferne genbruges (ingen 3–6 MB-allokering pr. billede). Afkoderen kan testes
  uden konsol: `veyamux-ps-decode-test <fil.h264|fil.hevc>`. Leveres til den samme GPU-vej som capture-kort. Lyd: Opus
  → 48 kHz stereo i en kø på højst 120 ms, som lydtråden læser i stedet for PulseAudio-optagelse.
- **Controller:** `GamepadInput::padState` (PlayStation-layout: evdev-knapper, D-pad, sticks, analoge triggere,
  DualSense/DS4-touchpadens klik fra dens egen evdev-enhed) → `chiaki_session_set_controller_state`, kun når menuen er
  lukket (ellers en tom tilstand). Rumble fra spillet → `EVIOCSFF`/`FF_RUMBLE` på controllerne.
- **Ikke med endnu:** forbindelse over internettet (PSN holepunch), mikrofon, gyro og touchpad-bevægelser.
- **Forsinkelse (F3-overlayet):**
  - **Netværk:** halvdelen af den rundtur, chiakis senkusha-test måler ved start (`session.rtt_us`).
  - **Afkodning:** fra en hel frame er modtaget, til billedet er afkodet (glidende gennemsnit).
  - **VeyAmux:** samme tal som for capture-kort (`LiveVulkan::pipelineMs`).
  - **Bitrate og tab:** `stream_connection.measured_bitrate`, `congestion_control.packet_loss` og tabte frames fra
    video-callbacken.
  - **Konsollens indkodning** kan ikke måles og vises som "+ konsol ~8 ms".
- Fejlsøgning: `VEYAMUX_PS_LOG=1` viser hele chiakis log.

## Retter billedrytmen (frame pacing repair)
Menu → *Frame generation* → *Ret billedrytmen* (`enh/pacingRepair`, slået fra). `src/FramePacingRepair.h` er taget fra
Veyra-NRVideo 2.2.0, som har den fra PushFPS (GPL-3.0-or-later, DosukaSOL og PushFPS-bidragyderne), uændret bortset fra
namespace. Mange konsolspil låst til 30 fps viser billederne 17/50 ms i stedet for 33/33; frame generation – især
Lossless Scaling, der arbejder ved præsentationen – arver det ujævne. Med frame generation til og gentagne billeder
smidt væk lægger reparationen en mindste kvadraters linje gennem de seneste 16 nye billeders ankomsttider og giver hvert
billede sin plads på en jævn rytme (højst én capture-opdatering fra ankomsten; kun når mønstret kan forklares som en
låst billedhastighed, ellers urørt). VeyAmux kan ikke vise et billede før det ankommer, så tiden får + én
capture-opdatering (`MediaEngine`: capture-tråden giver billedet til en pacer-tråd med forfaldstid; højst 2 i kø). Målt
på et 17/50-mønster: billederne afleveres 32,6–33,8 ms fra hinanden, aldrig før ankomst. F3 viser andelen flyttede
billeder.

## VRR-advarsel ved Lossless Scaling
lsfg-vk 2 har kun pacing-tilstanden `none`: det rigtige og det genererede billede præsenteres lige efter hinanden med
tvunget FIFO, og skærmens faste opdatering skal lægge dem jævnt; lsfg-vk's fejlsøgningsguide siger "Disable VRR". Med
VRR (KDE: Adaptiv synkronisering *Automatisk* = til i fuldskærm) klumper de sig sammen. `src/DisplayVrr.cpp` læser
`kscreen-doctor -o` (KDE; "Vrr: Automatic/Always/Never" pr. tændt skærm, AppImage'ens Qt-miljø fjernet) ved start og
hver gang Lossless Scaling vælges; er VRR til, vises en advarsel i Lossless Scaling-afsnittet og én besked. Andre
skriveborde: ukendt, ingen advarsel.

## Gratis frame generation (AMD FSR 3.1.6)
Tilstand 5 (`enh/frameGenMode=5`, vises altid ved siden af Lossless Scaling; med lsfg-vk indlæst genstarter VeyAmux uden det): `src/FsrFrameGen.cpp` – FidelityFX optical
flow + frame interpolation (FSR 3.1.6-effektkode fra SDK 2.4.0 i SDK 1.1.4-rammen, MIT; se `third_party/fidelityfx/VEYAMUX_README.md`) på det færdige billede, med VeyAmux' egen bevægelsesestimering som
"spil-bevægelsesvektorer", konstant uendelig dybde, beskyttelse af statiske elementer (`LxStatic`) og skarphed i fine
detaljer (`LxDetail`). **Lås HUD'en** (`enh/hudLock`, slået fra): `shaders/LxHudUpdate.hlsl` / `LxHudCompose.hlsl`
(fra Veyra 2.2.0 / PushFPS, GPL-3.0-or-later) lærer pr. pixel, hvor længe den har stået stille, og dens kontrast til
naboerne; i det genererede billede tages pixels, der har stået stille ca. et sekund og ligger på en kant, fra det rigtige
billede, så tekst, kort og målere ikke vrider sig. Kun capture-vejen: det genererede billede kopieres til et visningsbillede med det samme, det
rigtige et halvt capture-interval senere (`EnhanceChain::recordShowReal`, `QTimer` → `update()`). Testet: et objekt der
flytter 8 px pr. billede placeres præcist 4 px i mellembilledet. 120 Hz-reglen gælder også her.
(Byggefejl rettet: komponenterne skal defineres som `FFX_FI`/`FFX_OF`, ellers er shader-blobbene tomme.)

## Livestream (RTMP)
`Recorder::startStream`: H.264 (VA-API, ellers libx264) med konstant bitrate, FLV over RTMP/RTMPS via en
afsendertråd; er den mere end ~5 s bagud, droppes køen og der fortsættes fra næste keyframe. `rw_timeout` 5 s.
Twitch `rtmp://live.twitch.tv/app/<nøgle>`, YouTube `rtmp://a.rtmp.youtube.com/live2/<nøgle>` eller egen URL.
Kører samtidig med replay/optagelse (samme koder); startes ikke midt i en optagelse med anden koder-indstilling.

## Billedtids-overlay (F3)
Tider for hvert billede sat på skærmen (rigtige og genererede, `LiveVulkan::shownTimes`; filer: `onFrame`), vist som
graf over 2 s med fps, 1%-low (99. percentil), jitter (standardafvigelse), forsinkelse og tabte billeder.

## Automatisk HDR-lysstyrke
`PlayerBridge::edidPeakFromBytes`: CTA-861-udvidelsen i `/sys/class/drm/card*-<stik>/edid`, HDR Static Metadata
(udvidet tag 6), maks. luminans = 50 · 2^(CV/32) nits. Stikket findes via `QScreen::name()`. Testet med en syntetisk EDID.

## Lydudgang og lydstyrkeudligning
Udgang: PulseAudio/PipeWire-sink, skiftes mens der afspilles. Listen over udgange (og capture-lydkilder) hentes med
libpulse selv (`src/PulseDevices.cpp`). Tidligere blev `pactl` brugt, og når det program manglede, stod der kun "Systemets
standard".

**Lyd fra kilden**: spektrum-indikatoren (`src/AudioMeter.cpp`) får kildens lyd, før lydstyrke og slå-fra bliver lagt på:
- Lyden mixes til mono, og der laves en 1024-punkts FFT med Hann-vindue for hver 512 samples.
- Den viser 24 logaritmiske bånd fra 50 Hz til 16 kHz, med -66 til -6 dBFS som skala.
- Søjlerne stiger hurtigt og falder over ca. 0,18 s, og "Der kommer lyd" vises, hvis der var lyd inden for 0,5 s.

**Lyd i stream og optagelse**: lyden bliver taget fra til optagelse, replay og stream på samme sted, før lydstyrke og
slå-fra. Slå lyd fra (M) og lydstyrken gælder derfor kun højttalerne. Udligning: RMS over ~0,4 s trækkes mod
−20 dBFS (angreb 0,3 s, slip 3 s, ±12 dB) plus begrænser ved −1 dBFS. Kun det, man hører – optagelser får originalen.

## Start når capture-kortet tilsluttes
`veyamux --watch` (fra `~/.config/autostart/veyamux-watch.desktop`): inotify på `/dev`; når kortet med navnet fra sidst
(`cap/name`, sysfs) dukker op og VeyAmux ikke kører, startes `--fullscreen --capture /dev/videoN`. `--capture last` finder
også kortet igen, hvis det har fået et andet videoN-nummer.

## Instant replay, optagelse og skærmbilleder
**F9** gemmer de sidste 30/60/120 s, **F10** starter/stopper en optagelse, **F12** gemmer et PNG-skærmbillede
(`~/Videos/VeyAmux`, `~/Pictures/VeyAmux`). I den direkte capture-vej konverteres det viste billede på GPU'en til NV12
(`shaders/LxNv12.hlsl`, BT.709 limited, nedskalering til 720p/1080p/1440p) og kopieres asynkront til CPU'en med en
VkEvent pr. slot (som OBS-kopien), så visningen ikke venter. `src/Recorder.cpp` koder med FFmpeg: `hevc_vaapi` →
`h264_vaapi` → `libx264` (≤1440p), GOP 1 s, ingen B-billeder, tidsstempler fra capture-driverens tid (VFR, tidsbase
1/90000); lyd tappes fra lydløkken (48 kHz, før lydstyrke, tidsstemplet efter lydforsinkelsen) og kodes til AAC.
Pakkerne ligger i en ringbuffer (replay) og/eller skrives direkte til en fragmenteret MP4 (optagelse – kan afspilles,
selv hvis programmet lukkes brat). Replay gemmes fra et keyframe i en baggrundstråd. Målt: A/V-synk i testfil 0 ms
afvigelse. libva bundles ikke i AppImage'en (værtens libva finder værtens VA-driver).

## GPU-tid og latens
Vulkan-timestamps pr. trin (input, støjreduktion, kantudjævning, opskalering, finish, output) i hver frame-slot, læst når
slotten genbruges (ingen ventetid), udglattet. Menuen viser GPU-tid, CPU-tid og "i VeyAmux" (capture-driverens
tidsstempel → GPU-arbejde færdigt).

## Profiler og lydforsinkelse
Profiler gemmer billedindstillingerne og lydforsinkelsen under et navn (taster 1–9). Bundet til et signal
(`WxH@fps FOURCC`) vælges profilen automatisk, når det signal åbnes. Lydforsinkelse −300…+300 ms (capture: kun positiv –
lyden forsinkes med en forsinkelseslinje; filer: billedets tidsplan flyttes).

## Opdateringer
AppImage'en har opdateringsinfo `gh-releases-zsync|Nolfad|VeyAmux|latest|VeyAmux-x86_64.AppImage.zsync`
(`VEYAMUX_UPDATE_INFO` ved bygning). Læg både `.AppImage` og `.AppImage.zsync` op i hver GitHub-release; Gear Lever og
AppImageUpdate henter så kun ændringerne.

## VRR-tilstand
Et capture-kort optager altid med fast frekvens (VRR på kortet er kun HDMI-gennemføring), så et 45 fps-spil kommer som
billeder med 1, 1 og 2 signalbilleders afstand (16,7 / 16,7 / 33,3 ms ved 60 Hz) – også på en VRR-skærm ser det hakkende ud.
*Frame generation → Fra → VRR* sorterer gentagne billeder fra og holder hvert nyt billede, til der er gået ét
indholdsinterval (målt billedfrekvens, ellers et løbende gennemsnit) siden det forrige, højst ¾ interval. Simuleret
(45/40/50 fps i 60 Hz, 1 ms USB-jitter): uregelmæssighed i billedafstand 6,7–8,3 ms → 0,8–1,2 ms, ekstra forsinkelse i
gennemsnit 4–5,5 ms. VRR-tilstanden kører med vsync (kræver genstart, knap i menuen), så skærmen viser hvert billede helt,
når det præsenteres. Kræver *Adaptiv synkronisering* i KDE's skærmindstillinger og VeyAmux i fuldskærm. Kan ikke
kombineres med Lossless Scaling (lsfg-vk kræver fast skærmfrekvens).

## Sprog
Engelsk, dansk, tysk, spansk, fransk, svensk, norsk (bokmål) og russisk.
- **Automatisk:** sproget tages fra `LANGUAGE`, `LC_ALL`, `LC_MESSAGES` og `LANG` (`no`/`nn` → bokmål). Dansk vælges
  stadig også, når kun regionen er dansk.
- **På startskærmen:** sproget kan vælges, og valget gemmes som `ui/lang`.

Hvor teksterne ligger:
- **Brugerfladen:** `qml/Strings.js`.
- **Beskeder fra C++:** `i18n/messages.json`, slået op på den engelske tekst.
- **Manglende oversættelser** vises på engelsk.

## Lav latens (capture)
Samme princip som Windows-versionen: billedet forlader aldrig GPU'en. Rå YUYV/UYVY/NV12-frames uploades, farvekonverteres og
forbedres i Vulkan *direkte i Qt's egen frame* (samme Vulkan-enhed, ingen readback, ingen CPU-konvertering), og V4L2-køen
tømmes så kun det nyeste billede bruges ("mailbox"). Vsync er slået fra som standard (`VEYAMUX_VSYNC=1` slår det til).
MJPEG dekodes stadig på CPU'en.
- **Én kopi:** et råt V4L2-billede kopieres ikke ud af driverens buffer i capture-tråden. `RawFrame::ext` peger ind i
  den mmap'ede buffer, og `RawFrame::hold` (shared_ptr med en deleter, der laver `VIDIOC_QBUF`) holder den fra driveren,
  til billedet er erstattet; renderen kopierer den én gang direkte til Vulkans upload-buffer. Der bedes om 6 buffere
  (renderen kan holde to: det nyeste og det viste); med færre end 5 kopieres som før. Mappingen og fd'en deles med de
  holdte billeder (`CaptureSource::Mapping`): efter `close()` lægges en buffer ikke tilbage, og den unmappes først, når
  det sidste billede er væk. `VEYAMUX_CAPTURE_COPY=1` bruger den gamle vej med kopi.
- **Ingen allokering pr. billede:** capture-tråden og renderen bytter `RawFrame`-buffere (`LiveVulkan::submit` swapper).
- **Videofiler** går samme vej (`MediaEngine::open(..., rawToGpu)`): afkodede billeder sendes til `onLiveFrame` i
  takt med uret – 8-bit 4:2:0 limited range som NV12 (VA-API's NV12 kopieres som rækker, software via swscale),
  PQ-HDR som P010, alt andet som RGBA (swscale med farverum som før). Ikke for HLG eller ikke-kvadratiske pixels (de
  bruger den gamle vej med RGB-konvertering, offscreen-Vulkan og readback). Et spring i tidslinjen sætter
  `RawFrame::reset`, så de temporale trin starter forfra. `VEYAMUX_FILE_GPU=0` bruger den gamle vej.
Start-logik: programmet afprøver Vulkan-UI i en barneproces og gemmer resultatet; på Wayland prøves XWayland, før det
falder tilbage til software. `VEYAMUX_UI=software|vulkan` tvinger valg, `--reset-ui` nulstiller det gemte valg.

## Capture-kort
Tryk **C** (eller kamera-ikonet). Vælg enhed, tilstand (★ = anbefalet, ≥50 fps foretrækkes) og lydkilde. Start direkte fra terminal:
`./VeyAmux-x86_64.AppImage --capture /dev/video0`. Kilden antages at være limited-range (typisk for HDMI-capture).

## Vindue eller skærm som kilde (skærmdeling)
*Vindue eller skærm …* nederst i enhedslisten (`--capture screencast`) bruger skrivebordets skærmdelings-portal
(xdg-desktop-portal ScreenCast over D-Bus, `src/ScreenCastPortal.cpp`): CreateSession → SelectSources (vindue eller
skærm, markøren tegnet ind i billedet, `persist_mode` 2) → Start → OpenPipeWireRemote. Portalen giver en PipeWire-fd og
et node-id, som `src/ScreenCast.cpp` læser som en PipeWire-videostrøm: delt hukommelse (memfd) i RGBA/RGBx (sendes
uændret videre) eller BGRA/BGRx (byttes om til RGBA), beskæringsrektangel (VideoCrop) respekteres, lige mål. Kun det
nyeste billede bruges; derefter går det samme live-spor som et capture-kort (RawFrame RGBA → GPU).
- libpipewire-0.3 indlæses fra systemet ved kørsel (dlopen), så den altid passer til systemets PipeWire; AppImagen har
  ingen egen kopi. Byggeriet bruger kun headerne (`libpipewire-0.3-dev`).
- Billeder kommer kun, når noget ændrer sig, så stilhed er ikke "intet signal"; når vinduet lukkes, slutter strømmen
  (`PW_STREAM_FLAG_DONT_RECONNECT`) og VeyAmux skriver "Det delte vindue blev lukket".
- Billedfrekvensen måles (ContentProbe på grøn-kanalen); 120 Hz-reglen bruger kun den målte frekvens, ikke skærmens
  maksimum, som portalen oplyser.
- Valget gemmes som `restore_token` (`cap/scToken`), så `--capture last` kan genåbne samme vindue uden at spørge.
  Afbrydes dialogen, slettes tokenet.

## Fra PushFPS 4.1 (2026-11-21)
- **Dublet-fjernelse på hele billedet** (`PictureCompare` i `src/ContentProbe.cpp`): hver luma-sample sammenlignes med
  det sidste *nye* billede; en sample tæller som ændret ved mere end 3 niveauer, og billedet er en gentagelse, så længe
  højst ca. 5 pr. million samples afviger (41 ved 4K). Hver buffer fra kortet sammenlignes én gang i `probeRaw`, og
  resultatet både måler billedraten og afgør, om næste udleverede billede er en gentagelse (`newSinceDelivered_`).
  Test (`veyamux-probe-test dedup`): 4K, 30 fps i 60 Hz med kortstøj og kun et 12 px element i bevægelse: det gamle
  gitter rammer 68/119, det nye 119/119, ca. 1,1 ms pr. billede.
- **SMAA 1x** (medium/høj/ultra, `AaSmaa*`, opslagstabeller i `third_party/smaa`) som alternativ til FXAA.
- **Tekstur / klarhed / fjern dis** (Veyras `ColorDetail*`): blokstatistik ved ca. 135 rækker, separabel blur, apply
  i outputstørrelsen (`recordDetail`).
- **Sammenligningsklip** (`shaders/LxCompare.hlsl`): originalen (`lin_`, sidste rigtige billede før forbedring) og
  VeyAmux' billede (`out8_`) i fire layouts (delt, side om side, lodret 1080×1920, afsløring); mærkater tegnes af Qt
  og lægges på i shaderen. Lossless Scalings billeder kommer efter VeyAmux og er derfor ikke med.
- **Klipmarkører** (F6): `<optagelse>-markers.txt` med `H:MM:SS.t  Marker n`.
- **Tjek min opsætning** og **ydelsesrapport**: `src/PlayerBridgeSetup.cpp`.
- **Over skærmens frekvens** (`checkOverRefresh`): advarsel når spillets fps × multiplikator > skærmens Hz × 1,05 i 3 s.

## Remote Play: konsol via IP-adresse
`ps/manualHosts` (`adresse|1/0` for PS5/PS4) lægges som ekstra (unicast) adresser i chiaki-ng's discovery
(`setManualHosts`), så konsollen spørges direkte. Svaret på en direkte forespørgsel er et rigtigt svar for
connection tracking (en firewall slipper det igennem), hvad svaret på en broadcast ikke er. Svarer den aldrig, kan den
registreres direkte på adressen (`psRegister("ip:<adresse>")`); id'et bliver så konsollens MAC fra registreringen.

## Opdatering inde fra appen
`src/AppUpdate.cpp`: spørger `api.github.com/repos/Nolfad/VeyAmux/releases/latest`, henter `VeyAmux-x86_64.AppImage`
til `$APPIMAGE.download`, tjekker GitHubs SHA-256 for filen (ellers SHA-1 fra `.zsync`), gør den kørbar og `rename()`r
den over `$APPIMAGE`. Den kørende kopi fortsætter til genstart. Automatisk søgning højst hver 12. time
(`update/lastCheck`); `veyamux-update-test [install]` tester det.

## Miljøvariabler
`VEYAMUX_HDR_OUT=0` (slå HDR-output fra) · `VEYAMUX_HDR_MODE=hdr10|scrgb` · `VEYAMUX_GAMESCOPE=0` · `VEYAMUX_AUDIO_MS=<ms>` ·
`--fullscreen` · `--capture last` · `VEYAMUX_REC_SOFTWARE=1` (optag med libx264) · `VEYAMUX_FSR1_RCAS=<stop>` ·
`VEYAMUX_UI=software|vulkan` · `VEYAMUX_VSYNC=1` · `--reset-ui` · `VEYAMUX_LSFG=0` (slå lsfg-vk fra) · `VEYAMUX_DEBUG_FG=1` (logger frame generation og opskalering i den direkte capture-vej) · `VEYAMUX_NO_GPU=1` (slå Vulkan-kæden fra) · `VEYAMUX_HWDEC=0` (slå VAAPI fra) · `VEYAMUX_PS_SWDECODE=1` (Remote Play: software-afkodning) · `VEYAMUX_FILE_GPU=0` (videofiler: gammel CPU-vej) · `VEYAMUX_CAPTURE_COPY=1` (capture: kopi som før) · `VEYAMUX_DEDUP_SPARSE=1` (dublet-fjernelse med det gamle glisne gitter) · `VEYAMUX_NO_UPDATE_CHECK=1` (ingen automatisk søgning efter opdateringer) · `--tab=N` (åbn menuen på fane N, med `--panel`) · `VEYAMUX_VAAPI_DEVICE=/dev/dri/renderD128` · `VEYAMUX_VK_DEVICE=n` · `VEYAMUX_SHADER_DIR`

## Genvejstaster
Mellemrum afspil/pause · ←/→ ±5 s (Shift ±30 s) · ↑/↓ volumen · F fuldskærm · E forbedring til/fra · P panel · M lyd fra · O åbn ·
C capture · 1–9 profiler · F9 gem replay · F10 optag · F12 skærmbillede

## Arkitektur
`VkContext` (Vulkan, ét universelt descriptor-layout t→0, u→16, b→32, s→48 via DXC `-fvk-*-shift`) → `EnhanceChain` →
`MediaEngine` (demux/video/lyd/render-tråde) → `PlayerBridge`/`VideoItem` (Qt/QML).
Licens: GPL-3.0 (som det oprindelige Veyra-NRVideo, som VeyAmux bygger på). Tredjepart: se `THIRD_PARTY_NOTICES_LINUX.md`.

Testværktøjer i build-mappen: `veyamux-upscale-test <shaders> <in.rgba> <w> <h> <frames> <out.rgba> sgsr|fsr|fsr1|nis [skala]` (hele forbedringskæden, til
PSNR-sammenligning af opskaleringsmetoderne; `VEYAMUX_TEST_NR=tilstand,styrke` slår støjreduktion til).
