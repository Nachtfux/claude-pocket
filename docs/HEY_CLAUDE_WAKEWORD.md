# Eigenes Wake-Word „Hey Claude“ trainieren

Die App **Hey Claude** auf dem Echo Spot hört lokal mit
[openWakeWord](https://github.com/dscripka/openWakeWord) (ONNX Runtime) auf
ein Wake-Word. Ein fertiges „Hey Claude“-Modell gibt es nicht. Bis du eins
trainiert hast, nimmt die App das vortrainierte Modell **„Hey Jarvis“**
(`hey_jarvis_v0.1.onnx`). Der Spot reagiert dann also auf „Hey Jarvis“.

Diese Anleitung beschreibt, wie du mit dem offiziellen Trainingsweg von
openWakeWord ein eigenes `hey_claude.onnx` baust, es am Mac testest und auf
den Spot schiebst.

> Stand: openWakeWord **0.6.0** (PyPI) und `main`-Branch des Repos, gelesen
> im November 2026. Was ich nicht selbst prüfen konnte, ist mit
> **[nicht verifiziert]** markiert.

---

## Überblick

| Schritt | Wo | Dauer (grob) |
|---|---|---|
| 1. Daten erzeugen (Piper-TTS), augmentieren, trainieren | Google Colab mit GPU | Schnelltest ca. 1 h, volles Training mehrere Stunden **[nicht verifiziert]** |
| 2. `hey_claude.onnx` herunterladen | Colab → Mac | 1 min |
| 3. Mit eigenen Aufnahmen testen | Mac, `oww_reference.py` | 10 min |
| 4. Auf den Spot pushen, Schwelle einstellen | Mac → Spot per `adb` | 5 min |

Wie das Training funktioniert (laut README):

- openWakeWord nimmt eine feste Kette: Melspektrogramm → Googles
  Speech-Embedding-Modell (eingefroren) → kleiner Klassifikator. **Trainiert
  wird nur der kleine Klassifikator.** Deshalb laden App und Training dieselben
  `melspectrogram.onnx` und `embedding_model.onnx` aus dem Release v0.5.1.
- Die Positivbeispiele sind **100 % synthetisch**: Das englische
  Piper-TTS-Modell `en_US-libritts_r-medium` spricht deine Zielphrase in
  tausenden Stimmen. Du musst nichts selbst einsprechen.
- Für die Negativbeispiele gibt es drei Quellen:
  - ähnlich klingende Phrasen, die automatisch aus Phonemen erzeugt werden
  - deine eigenen `custom_negative_phrases`
  - ca. 2000 Stunden vorberechnete Features aus allgemeinem Audio (ACAV100M)
- Das Ergebnis ist eine `.onnx`-Datei mit wenigen hundert KB
  **[Größe nicht verifiziert]**.

---

## 1. Training in Google Colab (GPU)

Es gibt zwei offizielle Wege (README, Abschnitt *Training New Models*):

1. **Einfaches Colab-Notebook** mit Formular-Oberfläche:
   <https://colab.research.google.com/drive/1q1oe2zOyZp7UsB3jJiQ1IFn8z5YfjwEb?usp=sharing>.
   Laut README liefert es in „<1 hour“ ein Modell, „but the performance of the
   model may be low in some deployment scenarios“. Gut für einen ersten
   Versuch. **[nicht verifiziert]**: Den Inhalt dieses Notebooks konnte ich
   nicht abrufen. Ich weiß also nicht, ob es mehrere Zielphrasen und eigene
   Negativphrasen annimmt.
2. **Ausführliches Notebook** `notebooks/automatic_model_training.ipynb`. Es
   ist vollständig konfigurierbar, und darauf baut der Rest dieser Anleitung
   auf. In Colab öffnest du es über *Datei → Notebook öffnen → GitHub* mit
   `dscripka/openWakeWord` und `notebooks/automatic_model_training.ipynb`.

### 1.1 Laufzeit einrichten

- *Laufzeit → Laufzeittyp ändern → **T4 GPU*** (reicht in der kostenlosen
  Stufe). Das Notebook selbst spricht von einer „free Google Colab instance
  with a T4 GPU“.
- **Google Drive einbinden** und `output_dir` dorthin legen. Kostenlose
  Colab-Sitzungen brechen nach einer Weile ohne Aktivität ab. Erzeugte Clips
  bleiben so erhalten, und `--generate_clips` macht dort weiter, wo es
  aufgehört hat. Laut Notebook: „it will continue generating until the number
  of files meets the targets“.

  ```python
  from google.colab import drive
  drive.mount('/content/drive')
  ```

- Training läuft laut Notebook **nur unter Linux**, weil Piper das braucht.
  Colab erfüllt das. Am Mac selbst trainieren geht nicht ohne Weiteres.

> **Achtung, Abhängigkeiten [nicht verifiziert]:** Das Notebook pinnt alte
> Versionen, z. B. `tensorflow-cpu==2.8.1`, `onnx_tf==1.10.0` und
> `speechbrain==0.5.14`. Auf neueren Colab-Python-Versionen lassen sich
> manche davon vermutlich nicht mehr installieren. Für uns ist das meist
> egal: `train.py` speichert **zuerst das ONNX-Modell**
> (`torch.onnx.export`, opset 13) und konvertiert erst danach nach TFLite.
> Schlägt nur die TFLite-Konvertierung (`onnx_tf`/`tensorflow`) fehl, liegt
> `hey_claude.onnx` trotzdem schon im `output_dir`. TFLite brauchen wir nicht.
> Wenn andere Pakete scheitern, sind das die Auswege:
> - die Pins im Setup-Block lockern
> - in Colab eine ältere Laufzeitversion wählen, falls angeboten
> - auf einem Linux-Rechner mit NVIDIA-GPU trainieren
> - Hilfe in den Issues des Repos suchen

### 1.2 Notebook-Zellen ausführen

Führe die Zellen des offiziellen Notebooks der Reihe nach aus:

1. **Environment setup.** Klont `piper-sample-generator` und lädt das
   Piper-Modell `en_US-libritts_r-medium.pt`. Installiert openWakeWord aus dem
   Git-Repo (`pip install -e ./openwakeword`) und lädt
   `melspectrogram.onnx` / `embedding_model.onnx` (v0.5.1).
   *Hinweis:* Das Notebook klont `rhasspy/piper-sample-generator`, die
   Beispiel-Config verweist dagegen auf den Fork
   `dscripka/piper-sample-generator`. Nimm, was das Notebook macht.
2. **Download Data.** Lädt drei Dinge:
   - Raumimpulsantworten (MIT RIR)
   - Hintergrundgeräusche (ein Teil von AudioSet plus 1 h Free Music Archive)
   - die vorberechneten Features `openwakeword_features_ACAV100M_2000_hrs_16bit.npy`
     und `validation_set_features.npy` von HuggingFace

   Die Dateien sind mehrere GB groß. Für bessere Modelle empfiehlt das
   Notebook mehr Hintergrund-Audio (`n_hours` erhöhen, weitere
   AudioSet-`.tar`-Teile).
3. **Konfiguration.** Ersetze die Beispielzelle („hey sebastian“) durch die
   Zelle aus Abschnitt 1.3.
4. **Training** in drei Schritten:

   ```python
   !{sys.executable} openwakeword/openwakeword/train.py --training_config my_model.yaml --generate_clips
   !{sys.executable} openwakeword/openwakeword/train.py --training_config my_model.yaml --augment_clips
   !{sys.executable} openwakeword/openwakeword/train.py --training_config my_model.yaml --train_model
   ```

5. **Optionaler Schritt 4** (TFLite neu konvertieren): **überspringen**, wir
   brauchen nur ONNX.

### 1.3 Konfiguration für „Hey Claude“

Diese Zelle ersetzt die Zelle „Modify values in the config“ im Notebook. Die
Schlüssel stammen aus `examples/custom_model.yml` und sind dort dokumentiert:

```python
config = yaml.load(open("openwakeword/examples/custom_model.yml", 'r').read(), yaml.Loader)

config["model_name"] = "hey_claude"          # -> <output_dir>/hey_claude.onnx
config["target_phrase"] = [
    "hey claude",
    "hey clawed",
    "hey klohd",
    "hey clode",
    "hey klaude",                             # see the note on "hey cloud" below
]
config["custom_negative_phrases"] = [
    "hey klaus", "hey claudia", "hey clown", "hey cloud",   # drop "hey cloud" if you say "Klaude" like "Klau-de"
    "hey cleo", "hey chloe", "hey clara", "hey jarvis",
    "okay google", "hey google", "alexa", "hey siri",
    "hey", "claude", "okay", "hallo",
]
config["n_samples"] = 20000                  # positive training clips (yml: "minimum of 20,000 recommended")
config["n_samples_val"] = 2000
config["steps"] = 50000                      # yml default; the notebook demo uses 10000
config["augmentation_rounds"] = 1            # 2 = more variety without generating more clips
config["model_type"] = "dnn"
config["layer_size"] = 32
config["max_negative_weight"] = 1500
config["target_false_positives_per_hour"] = 0.2

out = "/content/drive/MyDrive/oww_hey_claude"   # persistent across Colab disconnects
config["output_dir"] = out
config["rir_paths"] = ["./mit_rirs"]
config["background_paths"] = ["./audioset_16k", "./fma"]
config["background_paths_duplication_rate"] = [1, 1]
config["false_positive_validation_data_path"] = "validation_set_features.npy"
config["feature_data_files"] = {"ACAV100M_sample": "openwakeword_features_ACAV100M_2000_hrs_16bit.npy"}
config["batch_n_per_class"] = {"ACAV100M_sample": 1024, "adversarial_negative": 50, "positive": 50}
config["piper_sample_generator_path"] = "./piper-sample-generator"

with open("my_model.yaml", "w") as f:
    yaml.dump(config, f)
```

#### Warum mehrere Schreibweisen als Zielphrase?

Mehrere Einträge in `target_phrase` ergeben laut `custom_model.yml` trotzdem
**ein** binäres Modell, das auf jede der Phrasen anspringt. Die Schreibweisen
helfen aus zwei Gründen:

- **Der TTS ist englisch.** Piper (`en_US-libritts_r-medium`) spricht
  „claude“ amerikanisch aus, wie „klawd“ /klɔːd/. Openwakeword unterstützt
  laut README nur Englisch.
- **Deutsche Sprecher sagen es anders.** Meist „Klohd“ /kloːt/, also eher
  französisch, mit hartem t am Ende. Manche lesen es auch deutsch als
  „Klau-de“.

Mit gezielten Falschschreibungen bringst du den englischen TTS dazu, diese
Vokale zu sprechen:

| Phrase | Wirkung |
|---|---|
| `hey claude` | Original, englische Aussprache „klawd“ |
| `hey clawed` | gleiche Aussprache auf anderem Weg. Im englischen Wörterbuch ein Homophon von „claude“, bringt also etwas Varianz, aber wenig Neues |
| `hey klohd`, `hey clode` | langes o, wie die meisten Deutschen „Claude“ sagen. Unbekannte Wörter laufen durch einen Phonemizer (`deep-phonemizer`), das Ergebnis ist nicht garantiert |
| `hey klaude` | deutsche Lesart „Klau-de“ **[Aussprache durch den TTS nicht verifiziert]** |

**Hör dir vor dem Training ein paar erzeugte Clips an.** Sie liegen in
`<output_dir>/hey_claude/positive_train/*.wav`. So prüfst du, ob „klohd“
wirklich wie „Klohd“ klingt und nicht wie Kauderwelsch:

```python
import glob, IPython.display as ipd
for f in sorted(glob.glob(f"{out}/hey_claude/positive_train/*.wav"))[:5]:
    ipd.display(ipd.Audio(f))
```

Klingt eine Variante falsch, nimm sie raus. Lösche dann die Clips in
`positive_train/` und `positive_test/` und erzeuge sie neu. Sonst bleiben
die alten drin, weil `--generate_clips` nur auffüllt.

Nimm vor allem die Varianten auf, die **du** tatsächlich sagst. Jede
zusätzliche Variante macht das Modell toleranter, also auch anfälliger für
Fehlauslöser.

#### Negativ- und „adversarial“ Phrasen

`train.py` erzeugt pro Zielphrase automatisch ähnlich klingende
Gegenbeispiele über Phonem-Überlappung (`generate_adversarial_texts`, nur
Englisch, Homophone ausgeschlossen). `custom_negative_phrases` kommen
**zusätzlich** dazu. Laut `custom_model.yml` sind sie „a good way to reduce
false positives if you notice that, in practice, certain words or phrases are
problematic“.

Begründung für die Liste oben:

| Gruppe | Warum |
|---|---|
| **hey klaus, hey claudia, hey clara, hey cleo, hey chloe, hey clown** | deutsche bzw. häufige Namen mit „Kl-“ nach „hey“, typisch im Haushalt |
| **hey cloud** | sehr nah an „claude“, kommt im Alltag vor („die Cloud“) |
| **okay google, hey google, alexa, hey siri** | andere Assistenten im Raum oder im Fernsehen. Der Spot soll nicht mitspringen. „alexa“ ist auf einem Echo-Gerät sowieso naheliegend |
| **hey jarvis** | damit sich das neue Modell klar vom Fallback abgrenzt |
| **hey, claude, okay, hallo** | Teilphrasen. Das Modell soll nur auf die ganze Phrase reagieren. (`train.py` erzeugt zusätzlich Teilphrasen, `include_partial_phrase=1.0`) |

**Konflikt „hey klaude“ und „hey cloud“:** Die deutsche Lesart „Klau-de“
klingt fast wie „cloud“. Beides gleichzeitig als positiv und negativ zu
trainieren, verwirrt das Modell. Entscheide dich:

- Du sagst „Klohd“: `hey klaude` aus `target_phrase` entfernen,
  `hey cloud` als Negativ behalten. Das ist die Empfehlung.
- Du sagst „Klau-de“: `hey klaude` behalten, `hey cloud` aus den Negativen
  streichen.

Deutsche Wörter in den Negativen spricht der TTS englisch aus. Das ist eine
Näherung. Was am Ende wirklich Fehlalarme auslöst, merkst du erst im echten
Betrieb (Abschnitt 4). Dann ergänzt du die Liste und trainierst neu.

#### Schritte statt Epochen

openWakeWord trainiert nicht in Epochen, sondern über **Schritte**
(`steps`). Dabei laufen „early-stopping and checkpoint averaging“, eine
Lernrate mit „cosine decay and multiple cycles“ und ein zyklisches Gewicht für
Negativbeispiele bis `max_negative_weight` (Notebook-Intro, `train.py`).

| Lauf | `n_samples` | `n_samples_val` | `steps` | Zweck |
|---|---|---|---|---|
| Schnelltest (Notebook-Demo-Werte) | 1000 | 1000 | 10000 | Prüfen, ob die Pipeline läuft. Das Notebook senkt dafür auch `target_accuracy` und `target_recall` |
| **Empfohlen** | 20000 | 2000 | 50000 | brauchbares Modell |
| Ausbau | 50000–100000 | 5000 | 50000–100000 | laut yml „often 100,000+ is best“, braucht viel Colab-Zeit und Speicher |

Erzeugt das Modell zu viele Fehlalarme, erhöhe `max_negative_weight`, zum
Beispiel 2000–3000. Das yml sagt dazu: „adjusting the maximum negative weight
may be necessary“. Erkennt es dich zu schlecht, erhöhe `n_samples` oder
`augmentation_rounds`.

### 1.4 Export (ONNX)

Nach `--train_model` liegt das Modell hier:

```
<output_dir>/hey_claude.onnx      # in this guide: /content/drive/MyDrive/oww_hey_claude/hey_claude.onnx
```

Das ONNX-Modell erwartet laut `train.py` die Form `[1, N, 96]`:

- N = Anzahl der Embedding-Frames, bei bis zu ca. 2 s Clip-Länge **16**,
  wie bei `hey_jarvis`.
- Bei längeren Phrasen wird N größer, denn `train.py` setzt die Länge auf
  den Median der Clips plus 750 ms, mindestens 2 s.

Die App liest N aus der Modell-Eingabeform. Du musst also nichts anpassen.

Lade die Datei aus Google Drive auf den Mac. Der Dateiname muss am Ende
**`hey_claude.onnx`** lauten.

---

## 2. Am Mac testen (`oww_reference.py`)

`android/hey-claude/tools/oww_reference.py` lässt dieselbe Pipeline wie die
App auf einer WAV-Datei laufen. Es gibt die Scores pro 80-ms-Frame aus, als
`official` (echtes openwakeword-Paket) und als `app` (Nachbau von
`FeatureBuffer.kt`).

```bash
# one-time setup (macOS, Python >= 3.9)
python3 -m venv ~/.venv/oww && source ~/.venv/oww/bin/activate
pip install onnxruntime numpy tqdm requests scipy scikit-learn
pip install --no-deps openwakeword==0.6.0

# record 5 s from the Mac microphone (ffmpeg: brew install ffmpeg)
# ":0" = first audio input; list devices with: ffmpeg -f avfoundation -list_devices true -i ""
ffmpeg -f avfoundation -i ":0" -t 5 -ac 1 -ar 16000 -sample_fmt s16 hey_claude_1.wav

# the mel/embedding models come from the app assets (created by the Gradle build)
cd android/hey-claude
python3 tools/oww_reference.py ~/hey_claude_1.wav --wake ~/Downloads/hey_claude.onnx --pad 1
```

`--pad 1` setzt 1 s Stille davor, damit die Aufwärmphase (N Frames) nicht in
dein Wort fällt. Fehlt `app/src/main/assets/models/`, erzeugst du den Ordner
einmal mit `./gradlew assembleDebug` oder gibst `--models <ordner>` mit
`melspectrogram.onnx` und `embedding_model.onnx` an.

Gute Testmenge:

- **Positiv:** 10 Aufnahmen „Hey Claude“ in deiner normalen Sprechweise,
  laut, leise, aus 2–3 m Entfernung und mit Fernseher im Hintergrund.
  Erwartet: Max-Score deutlich über der Schwelle, mindestens **2
  aufeinanderfolgende Frames** darüber, denn erst so löst die App aus.
- **Negativ:** „Hey Klaus“, „die Cloud“, „Okay Google“, „Alexa“, ein paar
  Minuten Podcast oder Fernsehton. Erwartet: keine 2 aufeinanderfolgenden
  Frames über der Schwelle.

Die letzte Zeile `max |app - official|` sollte bei ca. `1e-5` liegen. Dann
rechnet die App genau wie die Referenz.

---

## 3. Auf den Spot bringen

Die App sucht das Modell in dieser Reihenfolge:

1. `/sdcard/Android/data/ai.pocket.heyclaude/files/hey_claude.onnx` (per `adb push`)
2. `assets/models/hey_claude.onnx` (falls ins APK eingebaut)
3. Fallback `assets/models/hey_jarvis_v0.1.onnx`

```bash
# the app must have been started once so that its files/ folder exists;
# mkdir -p is harmless if it already does
adb shell mkdir -p /sdcard/Android/data/ai.pocket.heyclaude/files
adb push hey_claude.onnx /sdcard/Android/data/ai.pocket.heyclaude/files/hey_claude.onnx
```

Danach in der App **„Zuhören stoppen“** und **„Zuhören starten“** tippen. Das
Modell wird beim Start des Dienstes geladen. In der Statuszeile **„Modell:
…“** sollte jetzt `hey_claude` stehen und nicht mehr `hey_jarvis`.

Kann die App die Datei nicht laden (kaputt, falsche Form), protokolliert sie
das und nimmt den nächsten Kandidaten, also am Ende wieder „Hey Jarvis“. Zum
Nachsehen:

```bash
adb logcat | grep -i "wake model"
```

Zurück zu „Hey Jarvis“:

```bash
adb shell rm /sdcard/Android/data/ai.pocket.heyclaude/files/hey_claude.onnx
```

Danach den Dienst neu starten.

---

## 4. Schwelle einstellen

Das README sagt: „All of the included openWakeWord models were trained to
work well with a default threshold of `0.5` … you are encouraged to determine
the best threshold for your environment“. Die App startet ebenfalls mit
**0.5** und löst aus, wenn **2 Frames hintereinander** über der Schwelle
liegen.

1. In der App **„Zuhören stoppen“**. Der Mikrofon-Test braucht das Mikrofon
   für sich.
2. **Mikrofon-Test** öffnen und „Test starten“ tippen.
3. Mehrmals aus normaler Entfernung „Hey Claude“ sagen und die **Spitze** des
   Wake-Scores ablesen. Danach normal reden, Fernseher an, und auch dort die
   Spitze ablesen.
4. Den Regler **„Schwelle“** etwa in die Mitte zwischen beide Spitzen legen.
   Faustregeln:
   - Fehlalarme: Schwelle rauf, z. B. 0.6–0.8.
   - Spot reagiert nicht: Schwelle runter, z. B. 0.3–0.4.
   - Liegen beide Spitzen nah beieinander, hilft keine Schwelle. Dann neu
     trainieren: mehr Daten, problematische Wörter in
     `custom_negative_phrases`, höheres `max_negative_weight`.
5. „Zuhören starten“.

Das README nennt als Zielgröße „<0.5 false accepts per hour“ und „<5 % false
rejects“. Ein eigenes, synthetisch trainiertes Modell erreicht das nicht
automatisch. Teste es ein paar Tage im echten Betrieb.

---

## 5. Lizenz

- Der **Code** von openWakeWord steht unter **Apache 2.0**.
- Die **vortrainierten Modelle**, also auch `melspectrogram.onnx`,
  `embedding_model.onnx` und `hey_jarvis_v0.1.onnx` aus dem Release v0.5.1,
  stehen unter **CC BY-NC-SA 4.0**. Laut README liegt das an „the inclusion
  of datasets with unknown or restrictive licensing as part of the training
  data“.
- Ein eigenes `hey_claude.onnx` entsteht mit diesen Modellen und mit Daten
  gemischter Lizenz:
  - Piper-Stimme `en_US-libritts_r-medium`
  - ACAV100M-Features
  - AudioSet und FMA

  Behandle es deshalb ebenfalls als **nur für den privaten,
  nicht-kommerziellen Gebrauch**. **[Keine Rechtsberatung; die genaue
  Lizenzlage des selbst trainierten Modells habe ich nicht geprüft.]**
- Deshalb liegen **keine Modelle im Repo**. Gradle lädt die vortrainierten
  Modelle beim Build in den gitignorierten Ordner `app/src/main/assets/models/`.
  Committe auch dein `hey_claude.onnx` nicht.

---

## Quellen

- openWakeWord-README (Training, Schwelle, Lizenz, Sprachen):
  <https://github.com/dscripka/openWakeWord/blob/main/README.md>
- Trainings-Notebook:
  <https://github.com/dscripka/openWakeWord/blob/main/notebooks/automatic_model_training.ipynb>
- Beispiel-Konfiguration mit Erklärung aller Schlüssel:
  <https://github.com/dscripka/openWakeWord/blob/main/examples/custom_model.yml>
- Trainingsskript (Clip-Länge, ONNX-Export opset 13, Adversarial-Texte):
  `openwakeword/train.py` und `openwakeword/data.py` im PyPI-Paket
  `openwakeword==0.6.0`, gelesen per `pip download openwakeword --no-deps`
- Synthetische Daten: <https://github.com/dscripka/openWakeWord/blob/main/docs/synthetic_data_generation.md>
- Einfaches Colab-Notebook (laut README):
  <https://colab.research.google.com/drive/1q1oe2zOyZp7UsB3jJiQ1IFn8z5YfjwEb?usp=sharing>
  **[Inhalt nicht verifiziert]**
- Von der Community trainierte Modelle (Home Assistant, laut README):
  <https://github.com/fwartner/home-assistant-wakewords-collection>
  **[Inhalt nicht verifiziert; ein „Hey Claude“-Modell dort ist mir nicht bekannt]**
- Modelle v0.5.1: <https://github.com/dscripka/openWakeWord/releases/tag/v0.5.1>
