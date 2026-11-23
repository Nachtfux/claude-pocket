# Hey Claude (Android)

Kleine Begleit-App für den entsperrten **Echo Spot 2017** mit LineageOS 18.1
(Android 11). Sie hört lokal per [openWakeWord](https://github.com/dscripka/openWakeWord)
(ONNX Runtime) auf das Wake-Word, weckt den Bildschirm, startet die **offizielle
Claude-App** und tippt per Bedienungshilfe (AccessibilityService) automatisch
auf den Sprachmodus-Button. Endet das Gespräch, hört sie wieder zu.

Es läuft nichts in der Cloud, außer der Claude-App selbst – das Wake-Word wird
komplett auf dem Gerät erkannt.

## Bauen

Voraussetzungen: JDK 17+ und das Android SDK (Platform 34, Build-Tools), z. B.
über Android Studio. `ANDROID_HOME` setzen oder `local.properties` mit
`sdk.dir=…` anlegen.

```bash
cd android/hey-claude
./gradlew assembleDebug
# APK: app/build/outputs/apk/debug/app-debug.apk
```

Beim ersten Build lädt Gradle die openWakeWord-Modelle (`melspectrogram.onnx`,
`embedding_model.onnx`, `hey_jarvis_v0.1.onnx`) von den openWakeWord-Releases
nach `app/src/main/assets/models/`. Die Modelle stehen unter **CC BY-NC-SA 4.0**
und werden deshalb **nicht** ins Repo eingecheckt. Vorhandene Dateien werden
nicht erneut geladen.

## Installieren

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Danach `tools/spot/post-install.sh` ausführen (Overlay-Recht, Bedienungshilfe,
Mikrofon-Recht) – oder in der App die Buttons der Reihe nach antippen:

1. **Mikrofon erlauben**
2. **Bedienungshilfe aktivieren** → „Hey Claude“ einschalten
3. **Overlay-Recht erteilen** (nötig, damit Claude aus dem Hintergrund starten darf)
4. **Zuhören starten**

In der Claude-App einmal mit dem eigenen Claude-Abo einloggen.

## Eigenes Wake-Word „Hey Claude“

Ohne eigenes Modell reagiert die App auf **„Hey Jarvis“**. Ein selbst trainiertes
`hey_claude.onnx` (openWakeWord-Format) wird so aufs Gerät gelegt – ein Neubau
ist nicht nötig, nur den Dienst neu starten:

```bash
adb push hey_claude.onnx /sdcard/Android/data/ai.pocket.heyclaude/files/hey_claude.onnx
```

Alternativ beim Bauen nach `app/src/main/assets/models/hey_claude.onnx` legen.

## Feintuning

- **Schwelle** (Slider in der App, 0,2–0,9): niedriger = empfindlicher, mehr
  Fehlauslösungen. Mit dem **Mikrofon-Test** sieht man Pegel und Wake-Score live.
- Logs: `adb logcat -s HeyClaude`
- Zuhören von außen fortsetzen (Debug):
  `adb shell am broadcast -a ai.pocket.heyclaude.RESUME`
