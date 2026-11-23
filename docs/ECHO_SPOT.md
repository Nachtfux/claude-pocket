# Echo Spot (2017, „rook") → Claude-Lautsprecher

Komplette Anleitung für den **Mac**: Echo Spot entsperren, LineageOS 18.1
(Android 11) flashen, die **offizielle Claude-App** mit deinem Claude-Abo
nutzen und mit **Hey Claude** per Wake-Word direkt in den Sprachmodus springen.

Stand: 23.11.2026. Die Fakten zu amonet/TWRP/LineageOS stammen aus dem
XDA-Unlock-Thread, dem amonet-Quellcode (Branch `mt8163-rook`) und dem auf
echter Hardware getesteten spotdash-Guide. **Lies vor dem Entsperren trotzdem
den ersten Post des XDA-Threads** – dort steht die jeweils aktuelle Version.

> ⚠️ **Entsperren kann den Spot unbrauchbar machen.** Sobald der amonet-Payload
> läuft: **nichts anfassen, kein Kabel ziehen, kein Strom weg** (bis ~5 min).
> Du machst das auf eigenes Risiko.

---

## Überblick

```
 „Hey Claude"            Hey Claude (unsere App)                 Claude-App (offiziell)
 ───────────►  Mikrofon ─► openWakeWord (ONNX, lokal) ─► Bildschirm an,
                                                         Claude starten ──► Bedienungshilfe tippt
                                                                            auf den Sprachmodus-Button
                          ◄────────── Gespräch beendet / App verlassen ── hört wieder zu
```

- Das Wake-Word wird **komplett auf dem Spot** erkannt, es geht nichts in die Cloud.
- Die Claude-App bietet keinen offiziellen Deep-Link in den Sprachmodus,
  deshalb tippt eine Bedienungshilfe (AccessibilityService) den Button an.
- Ohne eigenes Modell reagiert Hey Claude zunächst auf **„Hey Jarvis"**
  (openWakeWord-Standardmodell). Ein eigenes „Hey Claude" trainierst du später.

Die Mac-Skripte liegen in [`tools/spot/`](../tools/spot/README.md). Sie laufen
mit der Standard-Bash von macOS (3.2), fragen vor jedem gefährlichen Schritt
nach und schreiben **nie** auf eine Partition – `preloader`, `lk` und `tee`
sind zusätzlich explizit gesperrt.

| Schritt | Skript | Spot-Zustand |
|---|---|---|
| Mac prüfen | `preflight.sh` | egal |
| Entsperren | `amonet-macos.sh` (ungetestet) / Windows `fastbrick.bat` | Fastboot |
| Backup | `backup.sh` | TWRP |
| LineageOS | `flash-lineage.sh` | TWRP |
| Claude-App prüfen | `inspect-claude-apk.sh` | LineageOS |
| Hey Claude einrichten | `post-install.sh` | LineageOS + USB-Debugging |

---

## 0. Checks vor dem Anstecken

- [ ] **Modell:** runder Spot mit Kamera, Modell **VN94DQ** (2017, Codename
      „rook"). Steht unten auf dem Gerät. Der Echo Spot von **2024 geht nicht**.
- [ ] **Fire-OS-Version:** Einstellungen → Geräteoptionen →
      Gerätesoftwareversion. amonet-rook unterstützt laut Quellcode genau
      **5.5.6.9, 5.5.5.2 und 5.5.3.4**. Notier die Version. Steht etwas
      anderes da: **nicht weitermachen**, erst im XDA-Thread nachlesen.
- [ ] Spot ab jetzt **nicht mehr ins WLAN / nicht anmelden** – ein Update kann
      die Lücke schließen.
- [ ] **Micro-USB-Datenkabel** (viele billige Kabel sind reine Ladekabel) und
      das **Original-Netzteil**. Der USB-Port sitzt hinten unter einer
      Abdeckung, aufschrauben musst du nichts.
- [ ] Am Mac den Spot möglichst **direkt** anschließen (USB-A-auf-C-Adapter
      statt Hub). MediaTek-Preloader-Modi sind zeitkritisch.

### Tastenkombinationen

| Kombination (beim Einstecken des Netzteils halten) | Ergebnis |
|---|---|
| **Alle drei Tasten oben** (Lauter + Leiser + Mute) – stock | Amazon-Fastboot „=> FASTBOOT mode..." (für den Unlock) |
| **Leiser** – nach dem Unlock | gehacktes Fastboot (amonet/kaeru) |
| **Lauter** – nach dem Unlock | TWRP-Recovery |
| **Mute + Leiser**, USB steckt schon | Preloader-Download-Modus (für `boot-recovery.sh` / `boot-fastboot.sh` und Notfälle) |

## 1. Mac vorbereiten

```bash
# Homebrew (falls noch nicht da): https://brew.sh
brew install android-platform-tools   # adb + fastboot
brew install bash coreutils           # amonet braucht bash >= 4 und GNU timeout
brew install python libusb            # python3 für amonet-Hilfsskripte, libusb für Notfall-Tools
python3 -m pip install --user pyserial
brew install scrcpy                   # optional: Spot-Bildschirm am Mac spiegeln

cd ~/claude-pocket                    # dein Klon dieses Repos
tools/spot/preflight.sh
```

`preflight.sh` meldet fehlende Tools mit dem passenden `brew`-Befehl, zeigt
angeschlossene USB-Geräte (MediaTek `0x0e8d`, Amazon `0x1949` werden markiert)
und sagt dir, was als Nächstes dran ist.

## 2. Downloads

| Was | Wo | Hinweis |
|---|---|---|
| **amonet-rook** (z. B. `amonet-rook-v2.0.0.zip`, ~40 MB) | Anhang im 1. Post von <https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-spot-2017-rook.4754878/> (XDA-Login nötig) | Quellcode: <https://github.com/R0rt1z2/amonet/tree/mt8163-rook> |
| **LineageOS 18.1 rook** (z. B. `lineage-18.1-20251108-UNOFFICIAL-rook.zip`, ~450 MB) | <https://github.com/amazon-oss/releases/releases> | **SHA256 aus dem Release notieren** |
| **Claude-Android-App** | als APK, oder später über **Aurora Store** (Play-Store-Client ohne Google-Konto) | Login mit deinem Claude-Konto |
| **Hey Claude** | selbst bauen, siehe [Abschnitt 8](#8-hey-claude-installieren) | – |

## 3. Entsperren + TWRP

### Was amonet macht

Der Unlock heißt *fastbrick*: Das Skript schickt im Amazon-Fastboot mit
`fastboot flash brick bin/fastbrick.img` einen präparierten Payload. Der
Fastboot-Aufruf „hängt" dabei absichtlich – nach 8 s ohne Antwort
(`timeout`-Exit-Code 124) gilt der Exploit als erfolgreich. Danach arbeitet
der Spot alleine weiter (Preloader-/LK-Exploit, kaeru, TWRP) und bootet nach
bis zu ~5 min in **TWRP**. Am Rechner wird dafür **nur fastboot** gebraucht,
kein Python.

Das Zip enthält außerdem `boot-recovery.sh` / `boot-fastboot.sh`: Die sprechen
per `python3` + `pyserial` über den seriellen MediaTek-Port
(`/dev/cu.usbmodem*`) mit dem Preloader und booten direkt TWRP bzw. Fastboot –
praktisch, wenn die Tasten nicht reagieren.

### Weg A – Windows (getestet)

Das ist der Weg, mit dem der Unlock tatsächlich getestet wurde.
Treiber: [Kindle-Fire-USB-Treiber](https://developer.amazon.com/docs/fire-tablets/connecting-adb-to-device.html#install_usb_driver)
(Fallback: [Google USB Driver](https://developer.android.com/studio/run/win-usb)).

1. Spot ans Netzteil, USB zum PC, **alle drei Tasten** halten bis
   „=> FASTBOOT mode..." erscheint. Kontrolle: `fastboot devices`.
2. amonet-Zip entpacken, im Ordner `fastbrick.bat` starten.
3. `YES` eintippen. Ab dann **nichts mehr anfassen**, bis TWRP erscheint.

### Weg B – macOS (⚠️ ungetestet)

amonet bringt ein Linux-Skript `fastbrick.sh` mit. Auf einem normalen Mac
läuft es **nicht direkt**, weil es

1. `declare -A` benutzt (braucht bash ≥ 4, macOS hat 3.2),
2. GNU `timeout` aufruft (gibt's auf macOS nur als `gtimeout` aus `coreutils`),
3. das mitgelieferte `bin/fastboot` startet – das ist ein **Linux**-Programm.

`tools/spot/amonet-macos.sh` behebt alle drei Punkte, ohne amonet zu
verändern: Es ersetzt `bin/fastboot(32)` durch Links auf das Homebrew-fastboot
(Originale bleiben als `*.orig`), legt `timeout → gtimeout` in den `PATH` und
startet `fastbrick.sh` mit der Homebrew-Bash.

```bash
unzip ~/Downloads/amonet-rook-v2.0.0.zip -d ~/Downloads/amonet-rook
# Spot: alle drei Tasten halten, Netzteil einstecken → "=> FASTBOOT mode..."
fastboot devices                                   # muss eine Seriennummer zeigen
tools/spot/amonet-macos.sh ~/Downloads/amonet-rook
```

Das Skript fragt zweimal nach `YES` (einmal unser Wrapper, einmal amonet).
**Vorher selbst reinschauen:** Öffne `fastbrick.sh` (bzw. `fastbrick.bat` →
`fastbrick.ps1`) im Zip und prüfe, dass es weiterhin nur `fastboot getvar …`
und `fastboot flash brick …` ausführt. Hat sich das in einer neueren Version
geändert (z. B. ein Python-Schritt über `/dev/cu.usbmodem*`), lass den
Wrapper weg und nimm Weg A oder C.

Nicht auf dem Mac: `bootrom-step.sh` (Notfall-Weg, `modules/main.py` liest
`/proc`) – dafür Linux.

### Weg C – Linux-Live-USB (Fallback)

Ubuntu-Live-Stick booten (Intel-Mac oder beliebiger PC; Apple-Silicon-Macs
booten keine Live-USBs), dann:

```bash
sudo apt install adb fastboot python3-serial     # ältere Ubuntu: android-tools-adb android-tools-fastboot
cd amonet-rook/amonet && sudo ./fastbrick.sh
```

### Nach dem Unlock

1. TWRP fragt nach Schreibzugriff: **„Keep Read Only"** wählen.
2. Am Mac: `adb devices` → Seriennummer mit Status `recovery`.

> 🚫 **Niemals** `preloader`, `lk` oder `tee` flashen/löschen. In TWRP zeigen
> `lk`, `tee1`, `tee2` absichtlich auf `/dev/null`; die echten Partitionen
> heißen `lk_real`, `tee1_real`, `tee2_real` und werden **nur gesichert**.

## 4. Backup (Pflicht!)

```bash
tools/spot/backup.sh --dry-run     # zeigt nur den Plan
tools/spot/backup.sh               # sichert nach ~/EchoSpot-Backup/spot-backup-<Datum>
```

Das Skript sucht das `by-name`-Verzeichnis **auf dem Gerät** (meist
`/dev/block/platform/mtk-msdc.0/by-name`, je nach Kernel auch eine Ebene
tiefer) und liest dort die Partitionstabelle (die Nummern `mmcblk0pN` können
sich von Guide zu Guide unterscheiden). Fehlen Kernpartitionen (`boot`,
`recovery`, `system`, `lk_real`, `tee1_real`, `tee2_real`), bricht es ab. Es
zieht jede Partition plus `mmcblk0boot0/1`, prüft Größe (`/proc/partitions`)
und SHA256 (Spot vs. Mac). `MANIFEST.txt` und die Markierung `COMPLETE`
entstehen **nur**, wenn alles stimmt - ein abgebrochenes Backup hinterlässt
nur `MANIFEST.txt.partial`, und `flash-lineage.sh` erkennt es nicht als
Backup an. `userdata` und `cache` werden
übersprungen (`--with-userdata` nimmt `userdata` mit).

Das Backup enthält **gerätespezifische Daten** (Seriennummern, Schlüssel):
privat halten, nicht ins Git, am besten zusätzlich auf einen USB-Stick.

## 5. LineageOS flashen

```bash
tools/spot/flash-lineage.sh ~/Downloads/lineage-18.1-20251108-UNOFFICIAL-rook.zip <SHA256-aus-dem-Release>
```

Das Skript

1. prüft SHA256 und Zip am Mac,
2. prüft, dass ein `rook` in TWRP hängt und ein Backup existiert,
3. führt durch den Wipe – **nur Dalvik/ART Cache, System, Data, Cache** (per
   TWRP-Kommandozeile oder von Hand: Wipe → Advanced Wipe). Klappt *System*
   nicht: in TWRP *Mount* → „Mount system partition read-only" abwählen, nochmal,
4. kopiert die Zip nach `/sdcard` und vergleicht den SHA256 **auf dem Spot**
   (alternativ `adb sideload`),
5. installiert und startet neu. Bietet TWRP die „TWRP-App" an: **ablehnen**.

## 6. Erster Start

- Lineage-Animation → direkt der Launcher. Einen Einrichtungsassistenten gibt
  es nicht, das ist normal. Der erste Start dauert ein paar Minuten.
- **USB-Debugging:** Einstellungen → Über das Telefon → 7× auf „Build-Nummer"
  → System → Entwickleroptionen → USB-Debugging an. Am Spot den
  RSA-Dialog bestätigen. Kontrolle: `adb devices` → `device`.
- **WLAN:** 2,4 und 5 GHz gehen, **WPA3 nicht** (WPA2 oder Mischmodus nehmen).
- Bequem bedienen: `scrcpy` spiegelt den runden 480×480-Bildschirm auf den Mac.

Bekannte Einschränkungen von LineageOS auf dem Spot: Bluetooth wackelig,
Kamera geht nicht, Mute-Taste = Power, SELinux permissive, Deep Sleep aus.
Die runde Anzeige schneidet Apps an den Ecken ab.

## 7. Claude-App + GO/NO-GO-Test

```bash
adb install claude.apk          # oder Split-Bundle: tools/spot/post-install.sh claude.apks - -
```

(Alternativ Aurora Store installieren und die Claude-App dort holen.)

App öffnen → mit **deinem Claude-Konto** anmelden → Mikrofon erlauben.

### ✅/❌ Manueller Test – bevor du Zeit in Hey Claude steckst

| Test | GO wenn … |
|---|---|
| App startet, Login klappt | Chat-Ansicht erscheint, Antworten kommen |
| Sprachmodus öffnen (Button neben dem Eingabefeld) | Sprachansicht erscheint ohne Fehlermeldung |
| **Mikrofon:** normal laut aus ~1 m sprechen | Claude reagiert auf das Gesagte |
| **Lautsprecher:** Claude antwortet | Antwort ist klar hörbar (Lautstärke mit den Tasten oben) |
| Gespräch beenden | App kehrt zum Chat zurück |

**NO-GO** (kein Ton, Mikrofon stumm, App stürzt ab oder verlangt ein neueres
Android): hier aufhören. Prüfe im XDA-ROM-Thread bzw. bei
`amazon-oss/releases`, ob ein neuerer Build Audio repariert, und teste dort
erneut. Mit `adb logcat | grep -i audio` siehst du, ob Audio-Fehler auftauchen.

### Gibt es einen direkten Weg in den Sprachmodus?

```bash
tools/spot/inspect-claude-apk.sh            # holt die APK (inkl. Splits) vom Spot
tools/spot/inspect-claude-apk.sh claude.apk # oder eine lokale Datei
```

Listet Paketname, Version, minSdk (inkl. „Android 11 OK?"), alle Activities
mit Intent-Filtern (Actions, Kategorien, Schemes/Hosts/Pfade), App-Shortcuts
und alles, was nach `VOICE_COMMAND`/`ASSIST`/voice aussieht. Die letzte Zeile
(`CONCLUSION: …`) sagt, ob ein offizieller Einstieg existiert. Beim ersten
Lauf legt es eine Python-venv mit androguard unter `~/.cache/heyclaude-venv`
an. Findet es nichts – der Normalfall –, übernimmt die Bedienungshilfe von
Hey Claude das Antippen.

## 8. Hey Claude installieren

### Bauen (am Mac)

```bash
brew install openjdk@17
brew install --cask android-commandlinetools      # oder Android Studio
sdkmanager "platforms;android-34" "build-tools;34.0.0" "platform-tools"
export JAVA_HOME="$(brew --prefix openjdk@17)"
export ANDROID_HOME="$(brew --prefix)/share/android-commandlinetools"

cd android/hey-claude
./gradlew assembleDebug        # lädt beim ersten Mal die openWakeWord-Modelle
```

Details: [`android/hey-claude/README.md`](../android/hey-claude/README.md).

### Einrichten

```bash
tools/spot/post-install.sh - android/hey-claude/app/build/outputs/apk/debug/app-debug.apk -
#                          ^ Claude-APK  ^ Hey-Claude-APK                         ^ Aurora-APK  („-" = überspringen)
```

`post-install.sh`

- installiert die übergebenen APKs (auch Split-Bundles `.apks`/`.xapk` oder Ordner),
- sucht das Claude-Paket (`com.anthropic.claude`; weicht es ab, in Hey Claude eintragen),
- gibt Hey Claude das **Mikrofon** (`pm grant … RECORD_AUDIO`) und das
  **Overlay-Recht** (`appops … SYSTEM_ALERT_WINDOW allow`) – nötig, damit
  Claude aus dem Hintergrund starten darf,
- schaltet die **Bedienungshilfe** `ai.pocket.heyclaude/.launch.VoiceModeA11yService`
  ein (hängt an bestehende an, überschreibt nichts),
- nimmt Hey Claude von der Akku-Optimierung aus (`deviceidle whitelist`),
- schaltet auf Nachfrage die **Displaysperre** ab und setzt das
  **Bildschirm-Timeout** (Standard 120 s, `SCREEN_TIMEOUT_MS=…`),
- startet Hey Claude.

Jetzt **„Hey Jarvis"** sagen → Bildschirm geht an, Claude öffnet sich, der
Sprachmodus startet. Nach dem Gespräch hört Hey Claude wieder zu (spätestens
nach 10 min als Sicherheitsnetz).

```bash
adb logcat -s HeyClaude                                              # Logs live
adb shell am start -a ai.pocket.heyclaude.MIC_TEST -n ai.pocket.heyclaude/.MainActivity   # Mikrofon-Test (Pegel + Wake-Score)
adb shell am broadcast -a ai.pocket.heyclaude.RESUME -p ai.pocket.heyclaude               # Zuhören von Hand fortsetzen
```

## 9. Eigenes Wake-Word „Hey Claude"

Training und Export eines eigenen openWakeWord-Modells beschreibt
[`docs/HEY_CLAUDE_WAKEWORD.md`](HEY_CLAUDE_WAKEWORD.md). Das fertige Modell
kommt ohne Neubau aufs Gerät:

```bash
adb push hey_claude.onnx /sdcard/Android/data/ai.pocket.heyclaude/files/hey_claude.onnx
```

Danach in Hey Claude „Zuhören stoppen" → „Zuhören starten". Die App zeigt
oben das geladene Modell an.

## 10. Fehlersuche

| Problem | Lösung |
|---|---|
| `fastboot devices` leer | Datenkabel? Direkt am Mac statt Hub? Drei Tasten **vor** dem Netzteil halten. `tools/spot/preflight.sh` zeigt, ob der Spot als Amazon (`0x1949`) auftaucht. |
| amonet: „eMMC is in permanent read-only mode" | Hardwaredefekt des Speichers, Spot ist unverändert. Nichts zu machen. |
| amonet: „Device mismatch" | Falsches Gerät/Payload – Modell und Zip prüfen. |
| TWRP: System lässt sich nicht wipen | *Mount* → „read-only" abwählen, erneut. |
| `backup.sh`: sha256 mismatch | Kabel/Port wechseln, nochmal. **Nicht flashen**, bevor alles grün ist. |
| Claude startet nicht nach dem Wake-Word | `adb logcat -s HeyClaude`. Steht dort „no launch intent", stimmt das Claude-Paket nicht (in der App setzen). Overlay-Recht prüfen: `adb shell appops get ai.pocket.heyclaude SYSTEM_ALERT_WINDOW`. |
| Claude startet, aber der **Sprachmodus-Button wird nicht gefunden** | Claude-App im Chat-Bildschirm offen lassen, dann `adb shell am broadcast -a ai.pocket.heyclaude.DUMP_NODES -p ai.pocket.heyclaude` und `adb logcat -s HeyClaude` ansehen: Dort stehen alle Button-Beschriftungen/IDs. Passenden Text in Hey Claude als **Muster für den Sprachmodus-Button** eintragen (Regex, Standard `(?i)voice\|sprach\|stimm\|sound ?wave\|audio`). |
| Bedienungshilfe nach Update aus | `tools/spot/post-install.sh - - -` erneut ausführen oder Einstellungen → Bedienungshilfen → Hey Claude. |
| Wake-Word löst zu oft / nie aus | Schwelle in der App anpassen; Mikrofon-Test zeigt den Score live. |
| Hört nach dem Gespräch nicht wieder zu | `adb shell am broadcast -a ai.pocket.heyclaude.RESUME -p ai.pocket.heyclaude`; spätestens nach dem Timeout (Standard 600 s) geht es von selbst weiter. |
| WLAN verbindet nicht | WPA3 wird nicht unterstützt – Router auf WPA2/WPA2+WPA3-Mischmodus. |

## 11. Notfall (Brick)

- Hängt der Spot, aber TWRP oder Fastboot kommen noch (Lauter/Leiser beim
  Einstecken): Backup zurückspielen ist in TWRP möglich – **niemals**
  `preloader`/`lk`/`tee` zurückschreiben.
- Tasten reagieren nicht: Spot vom Strom, **Mute + Leiser** halten, USB+Strom
  anstecken (Preloader-Modus) und `tools/spot/amonet-macos.sh <amonet-dir> boot-recovery`
  bzw. unter Linux `./boot-recovery.sh`.
- Komplett tot: Wiederherstellungsweg im XDA-Unlock-Thread (Option 2):
  **Linux**, Gehäuse öffnen, Testpunkt kurzschließen, `bootrom-step.sh`.
  Die Quellen nennen den Testpunkt widersprüchlich (TP30/TM18) – Platinenfoto
  im Thread prüfen. MTKClient kann evtl. helfen.
- Zurück zu Fire OS: `return-to-stock.sh` aus dem amonet-Paket (falls im Zip
  enthalten), Details im XDA-Thread.

---

## Quellen

- XDA: [Unlock/Root/TWRP/Unbrick Amazon Echo Spot 2017 (rook)](https://xdaforums.com/t/unlock-root-twrp-unbrick-amazon-echo-spot-2017-rook.4754878/)
- amonet-Quellcode, Branch `mt8163-rook`: <https://github.com/R0rt1z2/amonet/tree/mt8163-rook>
  (`fastbrick.sh`, `brick.sh` mit den unterstützten LK-Builds 5.5.6.9 / 5.5.5.2 / 5.5.3.4, `boot-recovery.sh`, `modules/`)
- LineageOS 18.1 für rook: <https://github.com/amazon-oss/releases/releases>
- spotdash-Guide `docs/rooting.md` (auf echter Hardware getestet): <https://github.com/afrugalpenguin/spotdash>
- TECHO5 (Alternative mit Alpine Linux/Home Assistant): <https://github.com/HuskerMinion/techo5>, <https://github.com/HuskerMinion/techo5-spot>
- openWakeWord: <https://github.com/dscripka/openWakeWord> (vortrainierte Modelle: CC BY-NC-SA 4.0)
- Android Platform-Tools: <https://developer.android.com/tools/releases/platform-tools>
- androguard: <https://github.com/androguard/androguard>
