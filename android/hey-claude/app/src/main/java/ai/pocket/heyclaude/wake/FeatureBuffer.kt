package ai.pocket.heyclaude.wake

/**
 * Pure (Android/ONNX-free) port of openWakeWord's streaming feature pipeline
 * (`openwakeword/utils.py` `AudioFeatures._streaming_features` + `Model.predict`, v0.6.0),
 * specialised for the case the app always uses: exactly one 1280-sample chunk per call.
 * See [OpenWakeWordDetector] for the constants and their reference citations.
 *
 * Per [process] call:
 *  1. raw audio: keep the previous [OVERLAP_SAMPLES] samples + the new chunk (1760 samples;
 *     only 1280 on the first call after a reset, exactly like the reference deque),
 *  2. melspectrogram of that slice (8 frames steady state, 5 on the first call),
 *     transformed with `x / 10 + 2`, appended to a 76-frame mel window that starts out
 *     filled with 1.0 (reference `np.ones((76, 32))`),
 *  3. one embedding of the last 76 mel frames (window 76, step 8 = one step per 80 ms),
 *     appended to a window of the last [embeddingFrames] embeddings,
 *  4. wake model on that embedding window -> score.
 *
 * Warm-up: the reference pre-fills its embedding history with embeddings of random noise and
 * then forces the first 5 predictions to 0. This port instead returns 0 (and skips the wake
 * model entirely) until [embeddingFrames] real embeddings exist. From that call on the
 * window is identical to the reference's, so scores match the reference bit-for-bit modulo
 * float rounding (same audio, same model).
 *
 * All buffers are allocated once; [process] does no allocation of its own.
 */
class FeatureBuffer(
    private val models: FeatureModels,
    /** Number of embedding frames the wake model consumes (its input shape[1], e.g. 16). */
    val embeddingFrames: Int,
) {
    init {
        require(embeddingFrames > 0) { "embeddingFrames must be > 0, was $embeddingFrames" }
    }

    private val raw = FloatArray(RAW_WINDOW_SAMPLES)
    private var rawLength = 0

    private val melOut = FloatArray(MAX_MEL_FRAMES_PER_CALL * MEL_BINS)
    private val melWindow = FloatArray(MEL_WINDOW_FRAMES * MEL_BINS)

    private val embedding = FloatArray(EMBEDDING_DIM)
    private val features = FloatArray(embeddingFrames * EMBEDDING_DIM)

    /** Embeddings produced since construction / the last [reset]. */
    var embeddingsProduced: Int = 0
        private set

    /** Mel frames the melspectrogram model returned on the last [process] call (for tests). */
    var lastMelFrames: Int = 0
        private set

    /** True once the embedding window holds only real (stream-derived) embeddings. */
    val isWarm: Boolean get() = embeddingsProduced >= embeddingFrames

    init {
        reset()
    }

    /** Processes exactly [CHUNK_SAMPLES] samples and returns the wake score (0 while warming up). */
    fun process(pcm: ShortArray): Float {
        require(pcm.size == CHUNK_SAMPLES) {
            "expected exactly $CHUNK_SAMPLES samples, got ${pcm.size}"
        }

        // 1. Raw audio: previous OVERLAP_SAMPLES (if any) + new chunk.
        val keep = minOf(rawLength, OVERLAP_SAMPLES)
        if (keep > 0) System.arraycopy(raw, rawLength - keep, raw, 0, keep)
        for (i in 0 until CHUNK_SAMPLES) raw[keep + i] = pcm[i].toFloat()
        rawLength = keep + CHUNK_SAMPLES

        // 2. Melspectrogram + transform, appended to the 76-frame window.
        val frames = models.melspectrogram(raw, rawLength, melOut)
        check(frames in 1..MAX_MEL_FRAMES_PER_CALL) { "melspectrogram returned $frames frames" }
        lastMelFrames = frames
        val newValues = frames * MEL_BINS
        for (i in 0 until newValues) melOut[i] = melOut[i] / 10f + 2f
        appendRows(melWindow, melOut, newValues)

        // 3. One embedding of the last 76 mel frames, appended to the feature window.
        models.embedding(melWindow, embedding)
        appendRows(features, embedding, EMBEDDING_DIM)
        if (embeddingsProduced < Int.MAX_VALUE) embeddingsProduced++

        // 4. Wake model (only once the window is fully populated).
        if (!isWarm) return 0f
        return models.wakeScore(features)
    }

    /** Clears raw audio, mel and embedding history (mel window back to 1.0 like the reference). */
    fun reset() {
        raw.fill(0f)
        rawLength = 0
        melOut.fill(0f)
        melWindow.fill(1f)
        embedding.fill(0f)
        features.fill(0f)
        embeddingsProduced = 0
        lastMelFrames = 0
    }

    /** Read-only snapshot of the current 76x32 mel window (tests/debugging). */
    fun melWindowSnapshot(): FloatArray = melWindow.copyOf()

    /** Read-only snapshot of the current embedding window (tests/debugging). */
    fun featuresSnapshot(): FloatArray = features.copyOf()

    /** Shifts [window] left by [count] values and copies `src[0 until count]` to its end. */
    private fun appendRows(window: FloatArray, src: FloatArray, count: Int) {
        if (count >= window.size) {
            System.arraycopy(src, count - window.size, window, 0, window.size)
        } else {
            System.arraycopy(window, count, window, 0, window.size - count)
            System.arraycopy(src, 0, window, window.size - count, count)
        }
    }

    companion object {
        const val SAMPLE_RATE = 16_000
        /** 80 ms @ 16 kHz; utils.py:418/433 (`% 1280`). */
        const val CHUNK_SAMPLES = 1280
        /** 3 hops of 160 samples; utils.py:397 (`[-n_samples-160*3:]`). */
        const val OVERLAP_SAMPLES = 160 * 3
        const val RAW_WINDOW_SAMPLES = CHUNK_SAMPLES + OVERLAP_SAMPLES
        /** melspectrogram.onnx output width; utils.py:271. */
        const val MEL_BINS = 32
        /** Embedding window; utils.py:165/440 (`np.ones((76, 32))`, `[-76 + ndx:ndx]`). */
        const val MEL_WINDOW_FRAMES = 76
        /** Mel frames per 1280-sample step (steady state); utils.py:229/438 (`8`, `-8*i`). */
        const val MEL_STEP_FRAMES = 8
        /** embedding_model.onnx output size ([n,1,1,96]). */
        const val EMBEDDING_DIM = 96
        /** Upper bound for the mel output of one call (1760 samples -> 8 frames; slack). */
        const val MAX_MEL_FRAMES_PER_CALL = RAW_WINDOW_SAMPLES / 160
        /** Reference default when a wake model has a dynamic input dim; model.py:156. */
        const val DEFAULT_EMBEDDING_FRAMES = 16
    }
}
