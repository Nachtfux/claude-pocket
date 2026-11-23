package ai.pocket.heyclaude.wake

import ai.onnxruntime.OnnxTensor
import ai.onnxruntime.OrtEnvironment
import ai.onnxruntime.OrtSession
import ai.onnxruntime.TensorInfo
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer

/**
 * [FeatureModels] backed by ONNX Runtime. Android-free (plain `ai.onnxruntime` API), so it can
 * also be exercised from a desktop JVM with the `com.microsoft.onnxruntime:onnxruntime` jar.
 *
 * Input tensors wrap pre-allocated direct buffers (no per-call copy of the input); each
 * session runs with a single intra-op thread and spinning disabled, which keeps the MT8163
 * from burning a core on busy-waits between the 80 ms steps.
 */
class OnnxFeatureModels(
    private val env: OrtEnvironment,
    melModel: ByteArray,
    embeddingModel: ByteArray,
    wakeModel: ByteArray,
) : FeatureModels, AutoCloseable {

    private val melSession: OrtSession
    private val embSession: OrtSession
    private val wakeSession: OrtSession

    private val melInputName: String
    private val embInputName: String
    private val wakeInputName: String

    /** Wake model input shape[1] (e.g. 16 for `[1,16,96]`); model.py:156. */
    val embeddingFrames: Int

    private val melInput = directFloats(FeatureBuffer.RAW_WINDOW_SAMPLES)
    private val embInput = directFloats(FeatureBuffer.MEL_WINDOW_FRAMES * FeatureBuffer.MEL_BINS)
    private val wakeInput: FloatBuffer

    private val embShape = longArrayOf(
        1, FeatureBuffer.MEL_WINDOW_FRAMES.toLong(), FeatureBuffer.MEL_BINS.toLong(), 1,
    )
    private val wakeShape: LongArray
    private val melShapeFull = longArrayOf(1, FeatureBuffer.RAW_WINDOW_SAMPLES.toLong())
    private val melShapeChunk = longArrayOf(1, FeatureBuffer.CHUNK_SAMPLES.toLong())

    init {
        val opts = OrtSession.SessionOptions().apply {
            setIntraOpNumThreads(1)
            setInterOpNumThreads(1)
            setExecutionMode(OrtSession.SessionOptions.ExecutionMode.SEQUENTIAL)
            setOptimizationLevel(OrtSession.SessionOptions.OptLevel.ALL_OPT)
            addConfigEntry("session.intra_op.allow_spinning", "0")
            addConfigEntry("session.inter_op.allow_spinning", "0")
        }
        val created = ArrayList<OrtSession>(3)
        try {
            melSession = env.createSession(melModel, opts).also { created += it }
            embSession = env.createSession(embeddingModel, opts).also { created += it }
            wakeSession = env.createSession(wakeModel, opts).also { created += it }
        } catch (t: Throwable) {
            created.forEach { runCatching { it.close() } }
            throw t
        } finally {
            opts.close()
        }
        melInputName = melSession.inputNames.first()
        embInputName = embSession.inputNames.first()
        wakeInputName = wakeSession.inputNames.first()

        val wakeInfo = wakeSession.inputInfo.getValue(wakeInputName).info as? TensorInfo
        val shape = wakeInfo?.shape ?: longArrayOf(1, -1, FeatureBuffer.EMBEDDING_DIM.toLong())
        require(shape.size == 3) { "wake model input must be rank 3 [1,n,96], is ${shape.contentToString()}" }
        require(shape[2] <= 0 || shape[2] == FeatureBuffer.EMBEDDING_DIM.toLong()) {
            "wake model expects ${shape[2]}-dim features, pipeline produces ${FeatureBuffer.EMBEDDING_DIM}"
        }
        embeddingFrames = if (shape[1] > 0) shape[1].toInt() else FeatureBuffer.DEFAULT_EMBEDDING_FRAMES
        wakeShape = longArrayOf(1, embeddingFrames.toLong(), FeatureBuffer.EMBEDDING_DIM.toLong())
        wakeInput = directFloats(embeddingFrames * FeatureBuffer.EMBEDDING_DIM)
    }

    override fun melspectrogram(samples: FloatArray, length: Int, out: FloatArray): Int {
        melInput.clear()
        melInput.put(samples, 0, length)
        melInput.flip()
        val shape = when (length) {
            FeatureBuffer.RAW_WINDOW_SAMPLES -> melShapeFull
            FeatureBuffer.CHUNK_SAMPLES -> melShapeChunk
            else -> longArrayOf(1, length.toLong())
        }
        // Output [1,1,frames,32] (utils.py:203 squeezes it to [frames,32]).
        val n = runFirstOutput(melSession, melInputName, melInput, shape, out)
        return n / FeatureBuffer.MEL_BINS
    }

    override fun embedding(melWindow: FloatArray, out: FloatArray) {
        embInput.clear()
        embInput.put(melWindow)
        embInput.flip()
        // Input [1,76,32,1], output [1,1,1,96] (utils.py:440-442).
        val n = runFirstOutput(embSession, embInputName, embInput, embShape, out)
        check(n == FeatureBuffer.EMBEDDING_DIM) { "embedding model returned $n values" }
    }

    override fun wakeScore(features: FloatArray): Float {
        wakeInput.clear()
        wakeInput.put(features)
        wakeInput.flip()
        // Input [1,n,96]; output [1,1] -> prediction[0][0][0] (model.py:314).
        OnnxTensor.createTensor(env, wakeInput, wakeShape).use { tensor ->
            wakeSession.run(mapOf(wakeInputName to tensor)).use { result ->
                val fb = (result.get(0) as OnnxTensor).floatBuffer
                return fb.get(0)
            }
        }
    }

    /** Runs [session] on one float tensor, copies its first output into [out], returns value count. */
    private fun runFirstOutput(
        session: OrtSession,
        inputName: String,
        input: FloatBuffer,
        shape: LongArray,
        out: FloatArray,
    ): Int {
        OnnxTensor.createTensor(env, input, shape).use { tensor ->
            session.run(mapOf(inputName to tensor)).use { result ->
                val fb = (result.get(0) as OnnxTensor).floatBuffer
                val n = fb.remaining()
                check(n <= out.size) { "model output ($n values) exceeds buffer (${out.size})" }
                fb.get(out, 0, n)
                return n
            }
        }
    }

    override fun close() {
        runCatching { melSession.close() }
        runCatching { embSession.close() }
        runCatching { wakeSession.close() }
    }

    private companion object {
        fun directFloats(n: Int): FloatBuffer =
            ByteBuffer.allocateDirect(n * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
    }
}
