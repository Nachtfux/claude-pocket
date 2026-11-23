package ai.pocket.heyclaude

import ai.pocket.heyclaude.launch.ClaudeLauncher
import ai.pocket.heyclaude.launch.VoiceModeA11yService
import ai.pocket.heyclaude.wake.OpenWakeWordDetector
import android.Manifest
import android.annotation.SuppressLint
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import java.io.File
import kotlin.math.roundToInt
import kotlin.math.sqrt

/**
 * Status + setup screen, laid out for the Echo Spot's round 480x480 display:
 * one centred column, big touch targets, generous side padding.
 */
class MainActivity : AppCompatActivity() {

    companion object {
        /** Set by BootReceiver: start the service from the foreground, then close. */
        const val EXTRA_AUTOSTART = "autostart"

        private const val THRESHOLD_MIN = 0.2f
        private const val THRESHOLD_MAX = 0.9f
        private const val THRESHOLD_STEP = 0.05f
    }

    private val ui = Handler(Looper.getMainLooper())

    private lateinit var scroll: ScrollView
    private lateinit var statusText: TextView
    private lateinit var permButton: Button
    private lateinit var serviceButton: Button
    private lateinit var a11yButton: Button
    private lateinit var overlayButton: Button
    private lateinit var thresholdLabel: TextView
    private lateinit var thresholdSeek: SeekBar
    private lateinit var testButton: Button
    private lateinit var testInfo: TextView
    private lateinit var levelBar: ProgressBar
    private lateinit var scoreBar: ProgressBar
    private lateinit var testSection: View

    // ---- Mic test (own AudioRecord + detector, only while the service is stopped) ----
    @Volatile private var testRunning = false
    @Volatile private var testLevel = 0f
    @Volatile private var testScore = 0f
    @Volatile private var testPeak = 0f
    @Volatile private var testModel: String? = null
    @Volatile private var testError: String? = null
    private var testThread: Thread? = null
    private var autostartPending = false
    private var micTestRequested = false

    private val requestMic = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        Log.i(Constants.TAG, "RECORD_AUDIO granted=$granted")
        if (granted && micTestRequested) startMicTest()
        refresh()
    }

    private val ticker = object : Runnable {
        override fun run() {
            refresh()
            ui.postDelayed(this, 200)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        autostartPending = intent?.getBooleanExtra(EXTRA_AUTOSTART, false) == true
        micTestRequested = intent?.action == Constants.ACTION_MIC_TEST
        setContentView(buildUi())
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (intent.getBooleanExtra(EXTRA_AUTOSTART, false)) autostartPending = true
        if (intent.action == Constants.ACTION_MIC_TEST) micTestRequested = true
    }

    override fun onResume() {
        super.onResume()
        if (autostartPending) {
            autostartPending = false
            // We are in the foreground now, so the mic FGS gets while-in-use access.
            if (hasMic()) {
                Prefs.setServiceEnabled(this, true)
                WakeWordService.start(this)
                ui.postDelayed({ if (!isFinishing) moveTaskToBack(true) }, 1_000)
            }
        }
        if (micTestRequested) {
            micTestRequested = false
            if (!WakeWordService.running) {
                if (hasMic()) startMicTest() else {
                    micTestRequested = true
                    requestMic.launch(Manifest.permission.RECORD_AUDIO)
                }
            }
            scroll.post { scroll.smoothScrollTo(0, testSection.top) }
        }
        ui.post(ticker)
    }

    override fun onPause() {
        ui.removeCallbacks(ticker)
        stopMicTest()
        super.onPause()
    }

    // ------------------------------------------------------------------
    // UI
    // ------------------------------------------------------------------

    private fun dp(v: Int): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics).roundToInt()

    private fun color(id: Int) = ContextCompat.getColor(this, id)

    private fun label(text: String = "", sizeSp: Float = 18f, bold: Boolean = false): TextView =
        TextView(this).apply {
            this.text = text
            setTextColor(color(R.color.ivory))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp)
            gravity = Gravity.CENTER
            if (bold) setTypeface(typeface, Typeface.BOLD)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(6) }
        }

    private fun button(text: String, onClick: () -> Unit): Button =
        Button(this).apply {
            this.text = text
            isAllCaps = false
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            setTextColor(color(R.color.dark))
            backgroundTintList = ColorStateList.valueOf(color(R.color.orange))
            minHeight = dp(56)
            setOnClickListener { onClick() }
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(8) }
        }

    private fun bar(): ProgressBar =
        ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            progressTintList = ColorStateList.valueOf(color(R.color.orange))
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(14),
            ).apply { topMargin = dp(4) }
        }

    private fun buildUi(): View {
        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            // Round screen: keep content away from the curved edges.
            setPadding(dp(56), dp(48), dp(56), dp(72))
        }

        col.addView(label(getString(R.string.app_name), 28f, bold = true).apply {
            setTextColor(color(R.color.orange))
        })
        statusText = label(sizeSp = 16f)
        col.addView(statusText)

        permButton = button(getString(R.string.btn_permission)) {
            requestMic.launch(Manifest.permission.RECORD_AUDIO)
        }
        col.addView(permButton)

        serviceButton = button(getString(R.string.btn_start)) { toggleService() }
        col.addView(serviceButton)

        a11yButton = button(getString(R.string.btn_a11y)) {
            openSettings(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
        }
        col.addView(a11yButton)

        overlayButton = button(getString(R.string.btn_overlay)) {
            openSettings(
                Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")),
            )
        }
        col.addView(overlayButton)

        // Threshold slider 0.2 .. 0.9 in 0.05 steps.
        thresholdLabel = label(sizeSp = 18f).apply {
            (layoutParams as LinearLayout.LayoutParams).topMargin = dp(20)
        }
        col.addView(thresholdLabel)
        val steps = ((THRESHOLD_MAX - THRESHOLD_MIN) / THRESHOLD_STEP).roundToInt()
        thresholdSeek = SeekBar(this).apply {
            max = steps
            progress = ((Prefs.threshold(this@MainActivity) - THRESHOLD_MIN) / THRESHOLD_STEP)
                .roundToInt().coerceIn(0, steps)
            progressTintList = ColorStateList.valueOf(color(R.color.orange))
            thumbTintList = ColorStateList.valueOf(color(R.color.orange))
            minimumHeight = dp(48)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT,
            )
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar, p: Int, fromUser: Boolean) {
                    if (fromUser) Prefs.setThreshold(this@MainActivity, progressToThreshold(p))
                    updateThresholdLabel()
                }

                override fun onStartTrackingTouch(sb: SeekBar) {}
                override fun onStopTrackingTouch(sb: SeekBar) {}
            })
        }
        col.addView(thresholdSeek)
        updateThresholdLabel()

        // Mic level / wake score test.
        val test = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(20) }
        }
        testSection = test
        test.addView(label(getString(R.string.test_title), 18f, bold = true))
        testButton = button(getString(R.string.btn_test_start)) {
            if (testRunning) stopMicTest() else startMicTest()
        }
        test.addView(testButton)
        test.addView(label(getString(R.string.test_level), 14f))
        levelBar = bar()
        test.addView(levelBar)
        test.addView(label(getString(R.string.test_score), 14f))
        scoreBar = bar()
        test.addView(scoreBar)
        testInfo = label(sizeSp = 14f)
        test.addView(testInfo)
        col.addView(test)

        col.addView(button(getString(R.string.btn_launch_test)) { launchTest() }.apply {
            (layoutParams as LinearLayout.LayoutParams).topMargin = dp(20)
        })

        scroll = ScrollView(this).apply {
            setBackgroundColor(color(R.color.dark))
            isFillViewport = true
            addView(col)
        }
        return scroll
    }

    private fun progressToThreshold(p: Int): Float =
        ((THRESHOLD_MIN + p * THRESHOLD_STEP) * 100).roundToInt() / 100f

    private fun updateThresholdLabel() {
        thresholdLabel.text = getString(R.string.threshold_label, Prefs.threshold(this))
    }

    private fun openSettings(i: Intent) {
        try {
            startActivity(i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        } catch (e: Exception) {
            Log.w(Constants.TAG, "Cannot open settings: ${i.action}", e)
        }
    }

    // ------------------------------------------------------------------
    // Status
    // ------------------------------------------------------------------

    private fun hasMic(): Boolean =
        ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED

    private fun a11yEnabled(): Boolean {
        val me = ComponentName(this, VoiceModeA11yService::class.java)
        val enabled = Settings.Secure.getString(contentResolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES)
            ?: return false
        return enabled.split(':').any { ComponentName.unflattenFromString(it.trim()) == me }
    }

    private fun claudeInstalled(): Boolean = try {
        packageManager.getPackageInfo(Prefs.claudePackage(this), 0)
        true
    } catch (e: PackageManager.NameNotFoundException) {
        false
    }

    private fun yes(b: Boolean) = getString(if (b) R.string.yes else R.string.no)

    private fun refresh() {
        val mic = hasMic()
        val running = WakeWordService.running
        val stateText = when {
            !running -> getString(R.string.state_stopped)
            else -> when (WakeWordService.state) {
                WakeWordService.State.LISTENING -> getString(R.string.state_listening)
                WakeWordService.State.PAUSED -> getString(R.string.state_paused)
                WakeWordService.State.ERROR -> getString(R.string.state_error)
                else -> getString(R.string.state_starting)
            }
        }
        val customModel = File(getExternalFilesDir(null), Constants.CUSTOM_MODEL_FILE).isFile
        val model = WakeWordService.modelName ?: testModel
            ?: if (customModel) Constants.CUSTOM_MODEL_FILE else getString(R.string.model_unknown)
        val a11y = a11yEnabled()
        val overlay = Settings.canDrawOverlays(this)
        val claude = claudeInstalled()

        val sb = StringBuilder()
        sb.append(stateText)
        if (running && WakeWordService.state == WakeWordService.State.LISTENING) {
            sb.append(String.format(" · %.2f", WakeWordService.lastScore))
        }
        sb.append('\n').append(getString(R.string.status_model, model))
        sb.append('\n').append(getString(R.string.status_mic, yes(mic)))
        sb.append('\n').append(getString(R.string.status_a11y, yes(a11y)))
        sb.append('\n').append(getString(R.string.status_overlay, yes(overlay)))
        sb.append('\n').append(getString(R.string.status_claude, yes(claude)))
        if (running && WakeWordService.micSilenced) {
            sb.append('\n').append(getString(R.string.status_silenced))
        }
        if (running) WakeWordService.lastError?.let { sb.append('\n').append(it) }
        statusText.text = sb.toString()

        permButton.visibility = if (mic) View.GONE else View.VISIBLE
        a11yButton.visibility = if (a11y) View.GONE else View.VISIBLE
        overlayButton.visibility = if (overlay) View.GONE else View.VISIBLE
        serviceButton.text = getString(if (running) R.string.btn_stop else R.string.btn_start)
        serviceButton.isEnabled = mic

        // Test section: own measurement while testing, otherwise mirror the service.
        val level: Float
        val score: Float
        if (testRunning) {
            level = testLevel
            score = testScore
            testInfo.text = testError ?: getString(R.string.test_info, testScore, testPeak)
        } else if (running) {
            level = WakeWordService.micLevel
            score = WakeWordService.lastScore
            testInfo.text = getString(R.string.test_service_running, WakeWordService.peakScore)
        } else {
            level = 0f
            score = 0f
            testInfo.text = testError ?: ""
        }
        // RMS of speech is small; scale so normal talking fills about half the bar.
        levelBar.progress = (sqrt(level.toDouble()) * 200).roundToInt().coerceIn(0, 100)
        scoreBar.progress = (score * 100).roundToInt().coerceIn(0, 100)
        testButton.text = getString(if (testRunning) R.string.btn_test_stop else R.string.btn_test_start)
        testButton.isEnabled = mic && !running
    }

    // ------------------------------------------------------------------
    // Actions
    // ------------------------------------------------------------------

    private fun toggleService() {
        if (WakeWordService.running) {
            Prefs.setServiceEnabled(this, false)
            WakeWordService.stop(this)
        } else {
            if (!hasMic()) {
                requestMic.launch(Manifest.permission.RECORD_AUDIO)
                return
            }
            stopMicTest()
            Prefs.setServiceEnabled(this, true)
            WakeWordService.start(this)
        }
        ui.postDelayed({ refresh() }, 300)
    }

    private fun launchTest() {
        stopMicTest()
        if (WakeWordService.running) {
            // Let the service release the mic and pause, exactly like a real trigger.
            WakeWordService.triggerNow(this)
        } else {
            val ok = ClaudeLauncher.launch(this)
            if (!ok) testError = getString(R.string.claude_missing, Prefs.claudePackage(this))
        }
    }

    @SuppressLint("MissingPermission")
    private fun startMicTest() {
        if (testRunning || WakeWordService.running) return
        if (!hasMic()) {
            micTestRequested = true
            requestMic.launch(Manifest.permission.RECORD_AUDIO)
            return
        }
        testError = null
        testPeak = 0f
        testRunning = true
        testThread = Thread({
            var rec: AudioRecord? = null
            var det: OpenWakeWordDetector? = null
            try {
                det = OpenWakeWordDetector(applicationContext)
                testModel = det.modelName
                val rate = 16_000
                val minBuf = AudioRecord.getMinBufferSize(
                    rate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
                )
                rec = AudioRecord(
                    MediaRecorder.AudioSource.VOICE_RECOGNITION, rate, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, maxOf(minBuf, rate * 2),
                )
                if (rec.state != AudioRecord.STATE_INITIALIZED) {
                    rec.release()
                    rec = AudioRecord(
                        MediaRecorder.AudioSource.MIC, rate, AudioFormat.CHANNEL_IN_MONO,
                        AudioFormat.ENCODING_PCM_16BIT, maxOf(minBuf, rate * 2),
                    )
                }
                if (rec.state != AudioRecord.STATE_INITIALIZED) {
                    testError = getString(R.string.test_mic_failed)
                    return@Thread
                }
                rec.startRecording()
                val frame = ShortArray(det.frameSamples)
                while (testRunning) {
                    var off = 0
                    while (off < frame.size && testRunning) {
                        val n = rec.read(frame, off, frame.size - off)
                        if (n < 0) {
                            testError = getString(R.string.test_mic_failed)
                            return@Thread
                        }
                        off += n
                    }
                    if (!testRunning) break
                    var sumSq = 0.0
                    for (s in frame) sumSq += (s * s).toDouble()
                    testLevel = (sqrt(sumSq / frame.size) / 32768.0).toFloat()
                    val score = det.process(frame)
                    testScore = score
                    if (score > testPeak) testPeak = score
                }
            } catch (e: Throwable) {
                Log.e(Constants.TAG, "Mic test failed", e)
                testError = "${getString(R.string.test_mic_failed)}: ${e.message}"
            } finally {
                try {
                    rec?.stop()
                } catch (_: Exception) {
                }
                rec?.release()
                try {
                    det?.close()
                } catch (_: Throwable) {
                }
                testRunning = false
                testLevel = 0f
                testScore = 0f
            }
        }, "HeyClaude-mictest").apply { start() }
    }

    private fun stopMicTest() {
        testRunning = false
        testThread?.let {
            try {
                it.join(1_000)
            } catch (_: InterruptedException) {
            }
        }
        testThread = null
    }
}
