#!/usr/bin/env python3
"""Per-frame openWakeWord scores for a WAV file, to compare against Hey Claude on-device.

Runs up to two pipelines over 80 ms (1280-sample) chunks of a 16 kHz mono int16 WAV:

  official  the real openwakeword package (Model.predict, ONNX backend) -- the reference.
  app       a numpy/onnxruntime mirror of the app's FeatureBuffer.kt (deterministic,
            no openwakeword install needed). From frame index N-1 on (N = wake model
            embedding frames, 16 for hey_jarvis) it must match `official` to ~1e-5.

Before that, `app` prints 0 (warm-up) while `official` prints 0 for 5 frames and then
scores computed over a random-noise-prefilled history (openwakeword utils.py:169).

Setup (macOS, any Python >= 3.9):
    python3 -m venv ~/.venv/oww && source ~/.venv/oww/bin/activate
    pip install onnxruntime numpy tqdm requests scipy scikit-learn
    pip install --no-deps openwakeword==0.6.0   # --no-deps: skips tflite-runtime (Linux only, unneeded)

Usage:
    python3 tools/oww_reference.py clip.wav                       # models from app assets
    python3 tools/oww_reference.py clip.wav --wake hey_claude.onnx
    python3 tools/oww_reference.py clip.wav --mode app --threshold 0.5

Convert a recording first if needed:
    ffmpeg -i in.m4a -ac 1 -ar 16000 -sample_fmt s16 clip.wav
"""
import argparse
import os
import sys
import wave

import numpy as np

CHUNK = 1280          # utils.py:418/433
OVERLAP = 160 * 3     # utils.py:397
MEL_BINS = 32         # utils.py:271
MEL_WINDOW = 76       # utils.py:165/440
EMB_DIM = 96

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MODELS = os.path.normpath(os.path.join(HERE, "..", "app", "src", "main", "assets", "models"))


def load_wav(path):
    with wave.open(path, "rb") as f:
        if f.getnchannels() != 1 or f.getsampwidth() != 2 or f.getframerate() != 16000:
            sys.exit(f"{path}: need 16 kHz mono 16-bit PCM WAV "
                     f"(got {f.getframerate()} Hz, {f.getnchannels()} ch, {8 * f.getsampwidth()} bit). "
                     "Convert: ffmpeg -i in -ac 1 -ar 16000 -sample_fmt s16 out.wav")
        return np.frombuffer(f.readframes(f.getnframes()), dtype=np.int16)


def chunks(audio):
    for i in range(0, len(audio) - CHUNK + 1, CHUNK):
        yield audio[i:i + CHUNK]


def run_official(audio, mel, emb, wake):
    from openwakeword.model import Model
    model = Model(wakeword_models=[wake], inference_framework="onnx",
                  melspec_model_path=mel, embedding_model_path=emb)
    name = list(model.models.keys())[0]
    return [float(model.predict(c)[name]) for c in chunks(audio)]


class AppMirror:
    """Line-by-line mirror of FeatureBuffer.kt + OnnxFeatureModels.kt."""

    def __init__(self, mel, emb, wake):
        import onnxruntime as ort
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = 1
        opts.inter_op_num_threads = 1
        prov = ["CPUExecutionProvider"]
        self.mel = ort.InferenceSession(mel, sess_options=opts, providers=prov)
        self.emb = ort.InferenceSession(emb, sess_options=opts, providers=prov)
        self.wake = ort.InferenceSession(wake, sess_options=opts, providers=prov)
        shape = self.wake.get_inputs()[0].shape
        self.n = shape[1] if isinstance(shape[1], int) and shape[1] > 0 else 16
        self.reset()

    def reset(self):
        self.raw = np.zeros(0, dtype=np.float32)
        self.mel_window = np.ones((MEL_WINDOW, MEL_BINS), dtype=np.float32)
        self.features = np.zeros((self.n, EMB_DIM), dtype=np.float32)
        self.produced = 0

    def process(self, pcm):
        assert len(pcm) == CHUNK
        keep = self.raw[-OVERLAP:] if len(self.raw) else self.raw
        self.raw = np.concatenate((keep, pcm.astype(np.float32)))
        spec = self.mel.run(None, {self.mel.get_inputs()[0].name: self.raw[None, :]})[0]
        spec = np.squeeze(spec).reshape(-1, MEL_BINS) / 10 + 2
        self.mel_window = np.vstack((self.mel_window, spec))[-MEL_WINDOW:].astype(np.float32)
        e = self.emb.run(None, {self.emb.get_inputs()[0].name: self.mel_window[None, :, :, None]})[0]
        self.features = np.vstack((self.features, e.reshape(1, EMB_DIM)))[-self.n:]
        self.produced += 1
        if self.produced < self.n:
            return 0.0
        out = self.wake.run(None, {self.wake.get_inputs()[0].name: self.features[None].astype(np.float32)})[0]
        return float(np.asarray(out).reshape(-1)[0])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav")
    ap.add_argument("--models", default=DEFAULT_MODELS, help="dir with melspectrogram.onnx / embedding_model.onnx")
    ap.add_argument("--wake", default=None, help="wake model .onnx (default: <models>/hey_claude.onnx or hey_jarvis_v0.1.onnx)")
    ap.add_argument("--mode", choices=["both", "official", "app"], default="both")
    ap.add_argument("--pad", type=float, default=0.0, help="seconds of leading silence to prepend")
    ap.add_argument("--threshold", type=float, default=0.5, help="mark frames above this score")
    ap.add_argument("--all", action="store_true", help="print every frame, not only frames with a score > 0.01")
    args = ap.parse_args()

    mel = os.path.join(args.models, "melspectrogram.onnx")
    emb = os.path.join(args.models, "embedding_model.onnx")
    wake = args.wake
    if wake is None:
        for cand in ("hey_claude.onnx", "hey_jarvis_v0.1.onnx"):
            if os.path.isfile(os.path.join(args.models, cand)):
                wake = os.path.join(args.models, cand)
                break
    for p in (mel, emb, wake):
        if not p or not os.path.isfile(p):
            sys.exit(f"model not found: {p} (run the Gradle download task or pass --models/--wake)")

    audio = load_wav(args.wav)
    if args.pad > 0:
        audio = np.concatenate((np.zeros(int(16000 * args.pad), dtype=np.int16), audio))

    cols = {}
    if args.mode in ("both", "official"):
        try:
            cols["official"] = run_official(audio, mel, emb, wake)
        except ImportError as e:
            msg = f"openwakeword unavailable ({e}); see setup in --help"
            if args.mode == "official":
                sys.exit(msg)
            print(f"# {msg} -- showing app mirror only", file=sys.stderr)
    app = None
    if args.mode in ("both", "app"):
        app = AppMirror(mel, emb, wake)
        cols["app"] = [app.process(c) for c in chunks(audio)]

    names = list(cols)
    n_frames = len(next(iter(cols.values())))
    print(f"# wav={args.wav} wake={os.path.basename(wake)} frames={n_frames} (80 ms each)")
    print("frame\tt_s\t" + "\t".join(names))
    for i in range(n_frames):
        vals = [cols[k][i] for k in names]
        if not args.all and max(vals) <= 0.01:
            continue
        mark = "  <-- above threshold" if max(vals) > args.threshold else ""
        print(f"{i}\t{(i + 1) * CHUNK / 16000:.2f}\t" + "\t".join(f"{v:.5f}" for v in vals) + mark)

    for k in names:
        s = np.array(cols[k])
        if len(s):
            print(f"# {k}: max={s.max():.5f} at frame {int(s.argmax())}, "
                  f"frames > {args.threshold}: {int((s > args.threshold).sum())}")
    if "official" in cols and "app" in cols and app is not None:
        start = app.n - 1
        a, o = np.array(cols["app"][start:]), np.array(cols["official"][start:])
        if len(a):
            print(f"# max |app - official| from frame {start}: {np.abs(a - o).max():.2e}")


if __name__ == "__main__":
    main()
