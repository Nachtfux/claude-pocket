package ai.pocket.heyclaude.wake

import ai.onnxruntime.OrtEnvironment
import ai.pocket.heyclaude.Constants
import android.content.Context
import android.util.Log
import java.io.File
import java.io.IOException

/**
 * openWakeWord on ONNX Runtime: melspectrogram.onnx -> embedding_model.onnx -> wake model.
 *
 * Faithful port of the streaming path of the Python reference, **openwakeword 0.6.0**
 * (`openwakeword/utils.py` = `AudioFeatures`, `openwakeword/model.py` = `Model`); the pure
 * windowing logic lives in [FeatureBuffer], the ONNX glue in [OnnxFeatureModels].
 *
 * Numeric constants (line numbers refer to the 0.6.0 wheel):
 *
 * | Constant | Value | Reference |
 * |---|---|---|
 * | sample rate | 16 000 Hz mono int16 | utils.py:41 (`sr=16000`), model.py:388 docstring |
 * | step / chunk | 1280 samples = 80 ms | utils.py:418, 433 (`% 1280`), model.py:388 (`chunk_size=1280`) |
 * | raw overlap fed to the mel model | 480 = 160*3 samples (input 1760; 1280 on the first call) | utils.py:397 (`list(self.raw_data_buffer)[-n_samples-160*3:]`) |
 * | mel model input | int16 values cast to float32, NOT normalised, shape [1, samples] | utils.py:194-202 |
 * | mel frames per call | 8 for 1760 samples, 5 for 1280 (`ceil(n/160 - 3)`, verified with onnxruntime) | utils.py:270 |
 * | mel bins | 32 | utils.py:271 |
 * | mel transform | `x / 10 + 2` | utils.py:180 (`melspec_transform = lambda x: x/10 + 2`), 206 |
 * | initial mel buffer | 76 x 32 of 1.0 | utils.py:165, 175 (`np.ones((76, 32))`) |
 * | embedding window / step | 76 mel frames, step 8 (one embedding per 1280 chunk = last 76 frames) | utils.py:225, 229, 437-442 (`ndx = -8*i`, `[-76 + ndx:ndx]`) |
 * | embedding model I/O | in `input_1` [1,76,32,1], out `conv2d_19` [1,1,1,96] | utils.py:92, 440; model file |
 * | embedding dim | 96 | embedding_model.onnx output |
 * | wake model input | last N embeddings, [1,N,96], N = input shape[1] (16 for hey_jarvis_v0.1) | model.py:156, utils.py:460 (`get_features`) |
 * | wake score | first output value, [1,1] -> `[0][0]` | model.py:314 (`prediction[0][0][0]`) |
 * | warm-up | reference: noise-filled history + first 5 predictions zeroed (utils.py:169, model.py:330-333); here: 0 until N real embeddings exist | see [FeatureBuffer] |
 *
 * Wake model lookup order (contract):
 *  1. `getExternalFilesDir(null)/hey_claude.onnx` ([Constants.CUSTOM_MODEL_FILE]; pushed via adb),
 *  2. `assets/models/hey_claude.onnx`,
 *  3. [Constants.FALLBACK_MODEL_ASSET] (`assets/models/hey_jarvis_v0.1.onnx`).
 * A candidate that exists but fails to load (corrupt file, wrong shape) is logged and skipped.
 *
 * Not thread-safe for concurrent [process] calls by design (one audio thread), but [process],
 * [reset] and [close] are synchronized so the service may close it from another thread.
 */
class OpenWakeWordDetector(context: Context) : WakeWordDetector {

    /** Human-readable name of the loaded wake model, e.g. `hey_claude (external)`. */
    val modelName: String

    /** Number of embedding frames the wake model consumes. */
    val embeddingFrames: Int

    override val frameSamples: Int = FeatureBuffer.CHUNK_SAMPLES

    private val models: OnnxFeatureModels
    private val buffer: FeatureBuffer
    private var closed = false

    init {
        val app = context.applicationContext ?: context
        val env = OrtEnvironment.getEnvironment()
        val mel = readAsset(app, MEL_ASSET)
        val emb = readAsset(app, EMBEDDING_ASSET)

        var loaded: Pair<String, OnnxFeatureModels>? = null
        var lastError: Throwable? = null
        for (candidate in wakeModelCandidates(app)) {
            val bytes = try {
                candidate.read() ?: continue
            } catch (e: IOException) {
                Log.w(TAG, "cannot read wake model ${candidate.name}", e)
                lastError = e
                continue
            }
            try {
                loaded = candidate.name to OnnxFeatureModels(env, mel, emb, bytes)
                break
            } catch (t: Exception) {
                Log.w(TAG, "cannot load wake model ${candidate.name}, trying next", t)
                lastError = t
            }
        }
        val (name, m) = loaded
            ?: throw IllegalStateException("no usable wake word model found", lastError)
        modelName = name
        models = m
        embeddingFrames = m.embeddingFrames
        buffer = FeatureBuffer(m, embeddingFrames)
        Log.i(TAG, "wake model: $modelName ($embeddingFrames embedding frames)")
    }

    @Synchronized
    override fun process(pcm: ShortArray): Float {
        check(!closed) { "detector is closed" }
        return buffer.process(pcm)
    }

    @Synchronized
    override fun reset() {
        if (!closed) buffer.reset()
    }

    @Synchronized
    override fun close() {
        if (closed) return
        closed = true
        models.close()
    }

    private class Candidate(val name: String, val read: () -> ByteArray?)

    private companion object {
        const val TAG = "OpenWakeWord"
        const val MEL_ASSET = "models/melspectrogram.onnx"
        const val EMBEDDING_ASSET = "models/embedding_model.onnx"

        fun readAsset(ctx: Context, path: String): ByteArray =
            ctx.assets.open(path).use { it.readBytes() }

        /** Returns null when the asset does not exist. */
        fun readOptionalAsset(ctx: Context, path: String): ByteArray? =
            try {
                ctx.assets.open(path)
            } catch (e: IOException) {
                null
            }?.use { it.readBytes() }

        fun baseName(path: String) = path.substringAfterLast('/').removeSuffix(".onnx")

        fun wakeModelCandidates(ctx: Context): List<Candidate> {
            val external = ctx.getExternalFilesDir(null)?.let { File(it, Constants.CUSTOM_MODEL_FILE) }
            val customAsset = "models/" + Constants.CUSTOM_MODEL_FILE
            return listOf(
                Candidate("${baseName(Constants.CUSTOM_MODEL_FILE)} (external)") {
                    external?.takeIf { it.isFile }?.readBytes()
                },
                Candidate("${baseName(customAsset)} (asset)") { readOptionalAsset(ctx, customAsset) },
                Candidate("${baseName(Constants.FALLBACK_MODEL_ASSET)} (fallback)") {
                    readAsset(ctx, Constants.FALLBACK_MODEL_ASSET)
                },
            )
        }
    }
}
