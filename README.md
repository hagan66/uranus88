# URANUS 88

En firmware för **M-VAVE FM-1** som gör om den till en åttastämmig analog polysynt i samma anda som 1981 års stora flaggskepp. Den bygger på [fm1-dock](https://github.com/joakimeriksson/fm1-dock) (Joakim Eriksson) och [Dinghy](https://github.com/zvenson/dinghy) (zvenson). Firmwaren går att installera på två sätt:

- som en **egen firmware** (`uranus88.fwsc`), där hela FM-1 blir en URANUS 88, eller
- som en **modul** (`uranus88.dock`) i fm1-dock, bredvid de andra syntarna där.

GPL-3.0. Projektet är inofficiellt och inte knutet till Roland eller M-VAVE. Du installerar firmware på egen risk.

## Ljudet

- **8 röster**: 4 på varje CPU-kärna. Med **AUTO** minskar antalet röster av sig självt om processorn inte hinner med.
- **VCO-1**: triangel, såg, puls och fyrkant. Har cross mod från VCO-2.
- **VCO-2**: sinus, såg, puls och brus. Har sync till VCO-1 och fininställning.
- **Lägen**: 64'–2' för båda VCO:erna.
- **VCO-modulator**: LFO och ENV-1 styr tonhöjden för VCO-1, VCO-2 eller båda. PWM styrs manuellt, av LFO eller av ENV-1.
- **Filter**: ett icke-resonant HPF följt av ett resonant 4-poligt lågpassfilter (24 dB, eller ett uttag på 12 dB) med mjuk mättning. Filtret självsvänger på högsta resonans.
- **Filtermodulation**: ENV-1 eller ENV-2, LFO, key follow och anslag.
- **Två ADSR**, med key follow på tiderna. ENV-2 styr VCA:n.
- **LFO**: triangel, såg, fyrkant och slump, med fördröjning per röst och VCA-modulation (tremolo).
- **ANALOG**: låter tonhöjd och filter glida lite olika i varje röst.
- **Key assign**: POLY1 (roterande), POLY2, UNISON, SOLO och CHORD, plus portamento.
- **VOICE DEPTH** som på en prologue: DUO i POLY (en andra, förstämd röst per tangent), sub-oscillatorer i SOLO (−1 och −2 oktaver), detune upp till 50 cent i UNISON och ackordtyp i CHORD (5th, sus2, m, Maj, sus4, m7, 7, 7sus4, Maj7, aug, dim, m7♭5, mMaj7, Maj7♭5).
- **Stereochorus** i BBD-stil med lägena I, II och I+II.
- **Arpeggiator**: UP, DOWN, U&D och RANDOM över 1–4 oktaver, med HOLD.
- **Stegsekvenser**: 16 steg som spelas in från tangenterna, med paus, bindning, gate, swing och transponering från tangenterna.
- **Split och dual** som på originalet: KEY MODE (GLO tre gånger) ger WHOLE, SPLIT (LOWER-ljudet under splitpunkten, panelens ljud ovanför) eller DUAL (båda på varje tangent), med fyra röster per ljud. LOWER är ett sparat ljud, 11–88, med egen oktav.
- **Två sekvensspår**: spår 1 spelas som tangenterna (genom splitten), spår 2 spelar alltid LOWER-ljudet. REC spelar in spår 1, REC igen spår 2.
- **808-trummaskin**: BD, SD, LT, CP, RS, CB, CH och OH med TUNE, DECAY och LEVEL för varje, åtta mönster om 16 steg med accentrad, på samma klocka som arpeggiator och sekvenser (swing också). Kanal 10 via MIDI spelar trummorna.
- **64 ljudplatser (11–88)**: 32 fabriksljud på 11–48 och 32 egna platser på 51–88.
- **MIDI in**: noter, pitch bend, modhjul och eftertryck (vibrato), sustain och programbyte. CC 74, 71, 73, 72 och 5 styr cutoff, reso, attack, release och portamento.

## Panelen

| | |
| --- | --- |
| KNOB 1–4 | sidans fyra reglage |
| SELECT | byter sida (18 sidor) |
| ALGORITHM | filtrets cutoff, på alla sidor |
| PRESETS | ljud 11–88 |
| EDIT | VCO-1 › VCO-2 › VCO MOD › MIXER |
| SEL | VCF › VCF MOD › VCA/KEY |
| ENV | ENV-1 / ENV-2 |
| LFO | LFO |
| FX | CHORUS (tryck igen för att byta läge) |
| GLO | KEY ASSIGN › GLOBAL (röster, stämning, sekvenserns tangentläge, stereobredd) › KEY MODE (WHOLE / SPLIT / DUAL, LOWER-ljud, splitpunkt, oktav) |
| ARP | arpeggiatorn på/av |
| SEQ | SEQUENCER › DRUM MACHINE › DRUM KIT. På trumsidorna är de 16 vita tangenterna steg och de svarta tangenterna väljer instrument (BD SD LT CP RS CB CH OH AC). |
| PLAY | startar/stoppar trummaskinen och sekvensern (DRUM MACHINE › PLAY väljer BOTH, DRUMS eller SEQ) |
| REC | stegsinspelning av spår 1, REC igen spår 2, REC igen sparar: tangenterna skriver steg, SEL ger en paus och EDIT en bindning |
| SAVE | tryck SAVE, välj plats med PRESETS och tryck SAVE igen. Andra knappar avbryter. |
| OCT− / OCT+ | oktav |
| HOME (håll) | fm1-dock:s väljare |

Säkerhetsnät från fm1-dock:
- Håll **OCT−** när du slår på FM-1 för att starta USB-räddningen, där du kan installera vilken firmware som helst.
- [FM-1 Firmware Hub](https://fm1.designburgapps.com) tar dig tillbaka till originalfirmwaren.

## Bygga

Kompilatorn för chippet (JieLi:s clang) finns bara för Linux x86-64. GitHub bygger firmwaren:

1. Workflowen `.github/workflows/build.yml` bygger vid varje push med Firmware Hubbens delade workflow ([zvenson/fm1-build](https://github.com/zvenson/fm1-build)). fm1-dock hämtas på en fast commit.
2. Ladda ner artefakten **packages** från körningen under fliken Actions. Där ligger `uranus88.fwsc`. En tagg `v1.0` publicerar den också som release, med en build attestation.

## Installera

1. **Prova först i webbläsaren.** Öppna [fm1-emu](https://joakimeriksson.github.io/fm1-emu) och välj `uranus88.fwsc`. Emulatorn kör paketet oförändrat.
2. **Gör en backup och ha vägen tillbaka redo.** Ladda ner M-VAVE:s V15 från m-vave.com/download.
3. **Installera.** Kör `pip3 install mido python-rtmidi` och sedan `python3 tools/fm1_install.py uranus88.fwsc` från en fm1-dock-checkout. Skriptet frågar innan det skriver något. Du kan också installera fm1-dock från [dess sida](https://joakimeriksson.github.io/fm1-dock/#install) och släppa `uranus88.dock` på konsolen där.

## Status

Ljudmotorn, skärmen och panelen är testade på en dator: som inbyggd kod med AddressSanitizer och UBSan, och som WebAssembly. Den har ännu inte körts på en riktig FM-1. Mät därför CPU-lasten där. Den syns på skärmens nedersta rad, och med VOICES AUTO tar firmwaren bort röster om den inte hinner med.

## Tack

- **fm1-dock** (värden och modulgränssnittet): Joakim Eriksson.
- **Dinghy**: zvenson.
- **Felucca**: Leo Kuroshita / Hügelton Instruments.
- **SLOOP**: isod89.
