package ai.pocket.heyclaude

import ai.pocket.heyclaude.launch.ClaudeLauncher
import ai.pocket.heyclaude.wake.OpenWakeWordDetector
import ai.pocket.heyclaude.wake.WakeWordDetector
import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import kotlin.math.abs
import kotlin.math.min
import kotlin.math.sqrt

/**
 * Foreground service that listens for the wake word.
 *
 * Loop (background thread): open AudioRecord -> feed frames of
 * detector.frameSamples -> on 2 consecutive frames above the threshold:
 * release the mic, reset the detector, launch Claude -> wait for
 * [Constants.ACTION_RESUME] (or the resume timeout) -> listen again.
 */
class WakeWordService : Service() {

    enum class State { STOPPED, STARTING, LISTENING, PAUSED, ERROR }

    companion object {
        const val ACTION_STOP = "ai.pocket.heyclaude.STOP"
        const val ACTION_TRIGGER_NOW = "ai.pocket.heyclaude.TRIGGER_NOW"

        private const val CHANNEL_ID = "listening"
        private const val NOTIFICATION_ID = 1
        private const val SAMPLE_RATE = 16_000
        private const val TRIGGER_FRAMES = 2
        /** Ignore triggers for the first frames after (re)start: ~1 s @ 80 ms. */
        private const val WARMUP_FRAMES = 12
        /** All-zero audio for this long => the OS is probably silencing us. */
        private const val SILENCE_WARN_MS = 5_000L
        private const val MAX_BACKOFF_MS = 30_000L

        // ---- State for MainActivity (polled from the UI thread) ----
        @Volatile var running = false
            private set
        @Volatile var state = State.STOPPED
            private set
        @Volatile var modelName: String? = null
            private set
        @Volatile var lastScore = 0f
            private set
        @Volatile var peakScore = 0f
            private set
        @Volatile var micLevel = 0f
            private set
        @Volatile var micSilenced = false
            private set
        @Volatile var lastError: String? = null
            private set
        @Volatile var triggerCount = 0
            private set
        @Volatile var lastTriggerAt = 0L
            private set

        fun start(ctx: Context) {
            try {
                ContextCompat.startForegroundService(ctx, Intent(ctx, WakeWordService::class.java))
            } catch (e: Exception) {
                Log.e(Constants.TAG, "Could not start WakeWordService", e)
                lastError = "Start fehlgeschlagen: ${e.message}"
            }
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, WakeWordService::class.java))
        }

        /** Same path as a detected wake word (releases the mic first). */
        fun triggerNow(ctx: Context) {
            val i = Intent(ctx, WakeWordService::class.java).setAction(ACTION_TRIGGER_NOW)
            try {
                ContextCompat.startForegroundService(ctx, i)
            } catch (e: Exception) {
                Log.e(Constants.TAG, "triggerNow failed", e)
            }
        }
    }

    private val lock = java.lang.Object()
    private val mainHandler = Handler(Looper.getMainLooper())
    @Volatile private var stopRequested = false
    @Volatile private var paused = false
    @Volatile private var resumeRequested = false
    @Volatile private var manualTrigger = false
    private var worker: Thread? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var receiverRegistered = false

    private val resumeReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == Constants.ACTION_RESUME) requestResume("broadcast")
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
        goForeground(getString(R.string.notif_starting))
        // Dynamic receivers are fine without export flags at targetSdk 30;
        // left exported so `adb shell am broadcast -a ai.pocket.heyclaude.RESUME` works for debugging.
        registerReceiver(resumeReceiver, IntentFilter(Constants.ACTION_RESUME))
        receiverRegistered = true
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "HeyClaude:listen").apply {
            setReferenceCounted(false)
            acquire()
        }
        running = true
        lastError = null
        setState(State.STARTING)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Every startForegroundService() must be answered with startForeground().
        goForeground(notificationText())
        when (intent?.action) {
            ACTION_STOP -> {
                Prefs.setServiceEnabled(this, false)
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_TRIGGER_NOW -> {
                if (paused) launchClaude() else manualTrigger = true
            }
        }
        ensureWorker()
        return START_STICKY
    }

    override fun onDestroy() {
        stopRequested = true
        synchronized(lock) { lock.notifyAll() }
        worker?.let {
            it.interrupt()
            try {
                it.join(1500)
            } catch (_: InterruptedException) {
            }
        }
        worker = null
        if (receiverRegistered) {
            try {
                unregisterReceiver(resumeReceiver)
            } catch (_: Exception) {
            }
            receiverRegistered = false
        }
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        mainHandler.removeCallbacksAndMessages(null)
        running = false
        lastScore = 0f
        micLevel = 0f
        state = State.STOPPED
        Log.i(Constants.TAG, "WakeWordService destroyed")
        super.onDestroy()
    }

    // ------------------------------------------------------------------
    // Worker
    // ------------------------------------------------------------------

    private fun ensureWorker() {
        if (worker?.isAlive == true) return
        stopRequested = false
        worker = Thread({ workerLoop() }, "HeyClaude-listen").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun workerLoop() {
        var detector: WakeWordDetector? = null
        var backoffMs = 1_000L
        try {
            while (!stopRequested) {
                if (detector == null) {
                    detector = try {
                        OpenWakeWordDetector(this).also {
                            modelName = it.modelName
                            Log.i(Constants.TAG, "Detector ready, model=${it.modelName}")
                        }
                    } catch (e: Throwable) {
                        Log.e(Constants.TAG, "Detector init failed", e)
                        fail("Modell konnte nicht geladen werden: ${e.message}")
                        sleepInterruptibly(backoffMs)
                        backoffMs = min(backoffMs * 2, MAX_BACKOFF_MS)
                        continue
                    }
                }

                if (paused) {
                    waitForResume()
                    continue
                }

                when (listen(detector)) {
                    ListenResult.TRIGGERED -> {
                        backoffMs = 1_000L
                        detector.reset()
                        launchClaude()
                    }
                    ListenResult.MIC_ERROR -> {
                        Log.w(Constants.TAG, "Mic error, retry in ${backoffMs} ms")
                        sleepInterruptibly(backoffMs)
                        backoffMs = min(backoffMs * 2, MAX_BACKOFF_MS)
                    }
                    ListenResult.HEALTHY_EXIT -> backoffMs = 1_000L
                    ListenResult.STOPPED -> Unit
                }
            }
        } finally {
            try {
                detector?.close()
            } catch (e: Throwable) {
                Log.w(Constants.TAG, "Detector close failed", e)
            }
        }
    }

    private enum class ListenResult { TRIGGERED, MIC_ERROR, HEALTHY_EXIT, STOPPED }

    @SuppressLint("MissingPermission")
    private fun openRecorder(): AudioRecord? {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            fail("Mikrofon-Berechtigung fehlt")
            return null
        }
        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
        )
        if (minBuf <= 0) {
            fail("AudioRecord.getMinBufferSize = $minBuf")
            return null
        }
        val bufBytes = maxOf(minBuf, SAMPLE_RATE * 2) // >= 1 s of audio
        for (source in intArrayOf(MediaRecorder.AudioSource.VOICE_RECOGNITION, MediaRecorder.AudioSource.MIC)) {
            try {
                val rec = AudioRecord(
                    source, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, bufBytes,
                )
                if (rec.state == AudioRecord.STATE_INITIALIZED) {
                    rec.startRecording()
                    if (rec.recordingState == AudioRecord.RECORDSTATE_RECORDING) {
                        Log.i(Constants.TAG, "AudioRecord started (source=$source, buf=$bufBytes)")
                        return rec
                    }
                    Log.w(Constants.TAG, "startRecording failed for source $source")
                    rec.stop()
                }
                rec.release()
            } catch (e: Exception) {
                Log.w(Constants.TAG, "AudioRecord source $source failed", e)
            }
        }
        fail("Mikrofon konnte nicht geöffnet werden")
        return null
    }

    private fun listen(detector: WakeWordDetector): ListenResult {
        val rec = openRecorder() ?: return ListenResult.MIC_ERROR
        val frame = ShortArray(detector.frameSamples)
        var consecutive = 0
        var frames = 0
        var silentSinceMs = -1L
        val startedAt = SystemClock.elapsedRealtime()
        detector.reset()
        lastError = null
        micSilenced = false
        peakScore = 0f
        setState(State.LISTENING)
        try {
            while (!stopRequested && !paused) {
                if (!readFully(rec, frame)) {
                    fail("Mikrofon-Lesefehler")
                    // A long healthy run before the error: retry quickly.
                    return if (SystemClock.elapsedRealtime() - startedAt > 10_000) {
                        ListenResult.HEALTHY_EXIT
                    } else {
                        ListenResult.MIC_ERROR
                    }
                }
                frames++

                // Level + "silenced by OS" detection.
                var peak = 0
                var sumSq = 0.0
                for (s in frame) {
                    val a = abs(s.toInt())
                    if (a > peak) peak = a
                    sumSq += (s * s).toDouble()
                }
                micLevel = (sqrt(sumSq / frame.size) / 32768.0).toFloat()
                val now = SystemClock.elapsedRealtime()
                if (peak == 0) {
                    if (silentSinceMs < 0) silentSinceMs = now
                    if (!micSilenced && now - silentSinceMs > SILENCE_WARN_MS) {
                        micSilenced = true
                        Log.w(Constants.TAG, "Mic delivers only zeros — capture probably silenced by the OS")
                    }
                } else {
                    silentSinceMs = -1L
                    micSilenced = false
                }

                val score = try {
                    detector.process(frame)
                } catch (e: Throwable) {
                    Log.e(Constants.TAG, "Detector process failed", e)
                    fail("Erkennung fehlgeschlagen: ${e.message}")
                    detector.reset()
                    return ListenResult.MIC_ERROR
                }
                lastScore = score
                if (score > peakScore) peakScore = score

                if (manualTrigger) {
                    manualTrigger = false
                    Log.i(Constants.TAG, "Manual trigger")
                    return ListenResult.TRIGGERED
                }

                val threshold = Prefs.threshold(this)
                consecutive = if (frames > WARMUP_FRAMES && score > threshold) consecutive + 1 else 0
                if (consecutive >= TRIGGER_FRAMES) {
                    Log.i(Constants.TAG, "Wake word detected (score=$score, threshold=$threshold)")
                    triggerCount++
                    lastTriggerAt = System.currentTimeMillis()
                    return ListenResult.TRIGGERED
                }
            }
            return if (stopRequested) ListenResult.STOPPED else ListenResult.HEALTHY_EXIT
        } finally {
            try {
                rec.stop()
            } catch (_: Exception) {
            }
            rec.release()
            micLevel = 0f
            lastScore = 0f
        }
    }

    private fun readFully(rec: AudioRecord, buf: ShortArray): Boolean {
        var off = 0
        while (off < buf.size) {
            if (stopRequested) return true
            val n = rec.read(buf, off, buf.size - off)
            if (n < 0) {
                Log.w(Constants.TAG, "AudioRecord.read returned $n")
                return false
            }
            off += n
        }
        return true
    }

    // ------------------------------------------------------------------
    // Pause / resume
    // ------------------------------------------------------------------

    /** Called on the worker thread after the mic has been released. */
    private fun launchClaude() {
        paused = true
        resumeRequested = false
        setState(State.PAUSED)
        mainHandler.post {
            val ok = try {
                ClaudeLauncher.launch(this)
            } catch (e: Exception) {
                Log.e(Constants.TAG, "ClaudeLauncher.launch threw", e)
                false
            }
            if (!ok) {
                lastError = "Claude-App nicht gefunden (${Prefs.claudePackage(this)})"
                Log.w(Constants.TAG, lastError!!)
                mainHandler.postDelayed({ requestResume("launch failed") }, 2_000)
            }
        }
    }

    private fun requestResume(reason: String) {
        if (!paused) return
        Log.i(Constants.TAG, "Resume requested ($reason)")
        resumeRequested = true
        synchronized(lock) { lock.notifyAll() }
    }

    private fun waitForResume() {
        val timeoutMs = Prefs.resumeTimeoutSec(this) * 1000L
        val deadline = SystemClock.elapsedRealtime() + timeoutMs
        synchronized(lock) {
            while (!stopRequested && !resumeRequested) {
                val left = deadline - SystemClock.elapsedRealtime()
                if (left <= 0) {
                    Log.i(Constants.TAG, "No RESUME within ${timeoutMs / 1000} s — resuming anyway")
                    break
                }
                try {
                    lock.wait(left)
                } catch (_: InterruptedException) {
                    if (stopRequested) return
                }
            }
        }
        resumeRequested = false
        paused = false
        // Give the Claude app a moment to release the microphone.
        sleepInterruptibly(500)
    }

    private fun sleepInterruptibly(ms: Long) {
        val deadline = SystemClock.elapsedRealtime() + ms
        synchronized(lock) {
            while (!stopRequested) {
                val left = deadline - SystemClock.elapsedRealtime()
                if (left <= 0) return
                try {
                    lock.wait(left)
                } catch (_: InterruptedException) {
                    return
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // State + notification
    // ------------------------------------------------------------------

    private fun fail(msg: String) {
        lastError = msg
        setState(State.ERROR)
    }

    private fun setState(s: State) {
        if (state == s) return
        state = s
        Log.i(Constants.TAG, "State -> $s")
        mainHandler.post {
            if (running) {
                val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
                nm.notify(NOTIFICATION_ID, buildNotification(notificationText()))
            }
        }
    }

    private fun notificationText(): String = when (state) {
        State.LISTENING -> getString(R.string.notif_listening)
        State.PAUSED -> getString(R.string.notif_paused)
        State.ERROR -> lastError ?: getString(R.string.notif_error)
        else -> getString(R.string.notif_starting)
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        val ch = NotificationChannel(
            CHANNEL_ID, getString(R.string.notif_channel), NotificationManager.IMPORTANCE_LOW,
        ).apply {
            setShowBadge(false)
        }
        nm.createNotificationChannel(ch)
    }

    private fun buildNotification(text: String): Notification {
        val openIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val stopIntent = PendingIntent.getService(
            this, 1,
            Intent(this, WakeWordService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_spark)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setContentIntent(openIntent)
            .addAction(0, getString(R.string.notif_action_stop), stopIntent)
            .build()
    }

    private fun goForeground(text: String) {
        val n = buildNotification(text)
        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        } else {
            0
        }
        ServiceCompat.startForeground(this, NOTIFICATION_ID, n, type)
    }
}
