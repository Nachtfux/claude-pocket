package ai.pocket.heyclaude

object Constants {
    const val PREFS = "hey_claude"

    /** Broadcast (setPackage(ctx.packageName)) => WakeWordService restarts listening. */
    const val ACTION_RESUME = "ai.pocket.heyclaude.RESUME"

    /** Activity intent action that opens MainActivity's mic level test. */
    const val ACTION_MIC_TEST = "ai.pocket.heyclaude.MIC_TEST"

    const val DEFAULT_CLAUDE_PACKAGE = "com.anthropic.claude"

    /** Looked up in context.getExternalFilesDir(null) first, then assets/models/. */
    const val CUSTOM_MODEL_FILE = "hey_claude.onnx"

    const val FALLBACK_MODEL_ASSET = "models/hey_jarvis_v0.1.onnx"

    /** Log tag used across the app. */
    const val TAG = "HeyClaude"
}
