package ai.pocket.heyclaude.wake

/**
 * The three neural networks of the openWakeWord pipeline, abstracted so that [FeatureBuffer]
 * (all windowing / ring-buffer logic) has no dependency on Android or ONNX Runtime and can be
 * unit-tested on a plain JVM with a fake implementation.
 *
 * Implementations may keep and reuse internal buffers; they are only ever called from one
 * thread at a time.
 */
interface FeatureModels {
    /**
     * Runs melspectrogram.onnx on `samples[0 until length]`.
     *
     * The samples are int16 PCM values cast to float WITHOUT normalisation (the reference
     * feeds `x.astype(np.float32)` of int16 data).
     *
     * Writes the raw (untransformed) model output, row-major `frames x 32`, into [out] and
     * returns the number of frames written. [out] holds at least
     * [FeatureBuffer.MAX_MEL_FRAMES_PER_CALL] frames.
     */
    fun melspectrogram(samples: FloatArray, length: Int, out: FloatArray): Int

    /**
     * Runs embedding_model.onnx on one window of `76 x 32` transformed mel frames (row-major,
     * oldest frame first) and writes the 96-dim embedding into [out].
     */
    fun embedding(melWindow: FloatArray, out: FloatArray)

    /**
     * Runs the wake-word model on `n x 96` embeddings (row-major, oldest first, n = the
     * model's embedding frame count) and returns its first output (score 0..1).
     */
    fun wakeScore(features: FloatArray): Float
}
