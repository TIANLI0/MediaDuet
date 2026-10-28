package app.mediaduet.unlock

import android.app.KeyguardManager
import android.content.Context
import android.content.Intent
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.os.Build
import android.os.Bundle
import android.os.CancellationSignal
import android.os.SystemClock
import androidx.activity.ComponentActivity
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import app.mediaduet.R
import app.mediaduet.service.AppLanguage
import app.mediaduet.service.Engine
import app.mediaduet.util.MdLog
import kotlinx.coroutines.launch

/**
 * Transparent activity that shows the system's biometric prompt (device PIN as fallback) for the
 * open unlock / enroll prompt ([UnlockPrompter.current]). It shows over the lock screen and turns
 * the screen on, so a locked phone needs one fingerprint. Closes when the prompt ends on the PC's
 * side (cancel, timeout, a newer request).
 */
class UnlockActivity : ComponentActivity() {
    private val prompter get() = Engine.get(this).unlock.prompter

    /** Token of the prompt the system dialog is showing (0: none). */
    private var showing = 0L
    private var signal: CancellationSignal? = null
    private var shownAt = 0L
    private var keyguardRetried = false

    override fun attachBaseContext(newBase: Context) {
        super.attachBaseContext(AppLanguage.wrap(newBase))
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setShowWhenLocked(true)
        setTurnScreenOn(true)
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                prompter.current.collect { p ->
                    when {
                        p == null -> close()
                        p.token != showing -> show(p)
                    }
                }
            }
        }
    }

    private fun show(p: UnlockPrompter.Prompt) {
        signal?.cancel()
        showing = p.token
        shownAt = SystemClock.elapsedRealtime()
        val enroll = p.kind == UnlockPrompter.Kind.ENROLL
        val builder = BiometricPrompt.Builder(this)
            .setTitle(getString(if (enroll) R.string.unlock_enroll_prompt_title else R.string.unlock_prompt_title, p.pcName))
            .setSubtitle(getString(if (enroll) R.string.unlock_enroll_prompt_text else R.string.unlock_prompt_text, p.pcName))
            .setConfirmationRequired(false)
        if (Build.VERSION.SDK_INT >= 30) {
            builder.setAllowedAuthenticators(BiometricManager.Authenticators.BIOMETRIC_STRONG or BiometricManager.Authenticators.DEVICE_CREDENTIAL)
        } else {
            builder.setNegativeButton(getString(R.string.cancel), mainExecutor) { _, _ -> done(p.token, UnlockPrompter.Outcome.Failed(UnlockErrors.DENIED)) }
        }
        val cancel = CancellationSignal().also { signal = it }
        try {
            builder.build().authenticate(BiometricPrompt.CryptoObject(p.cipher), cancel, mainExecutor, Callback(p.token, cancel))
        } catch (e: Exception) {
            MdLog.e(TAG, "cannot show the biometric prompt", e)
            done(p.token, UnlockPrompter.Outcome.Failed(UnlockErrors.NO_AUTH))
        }
    }

    private inner class Callback(private val token: Long, private val cancel: CancellationSignal) : BiometricPrompt.AuthenticationCallback() {
        override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) {
            done(token, UnlockPrompter.Outcome.Authenticated)
        }

        override fun onAuthenticationError(errorCode: Int, errString: CharSequence?) {
            if (cancel.isCanceled || token != showing) return // we closed it
            MdLog.i(TAG, "biometric prompt error $errorCode: $errString")
            when (errorCode) {
                BiometricPrompt.BIOMETRIC_ERROR_USER_CANCELED ->
                    done(token, UnlockPrompter.Outcome.Failed(UnlockErrors.DENIED))
                BiometricPrompt.BIOMETRIC_ERROR_NO_BIOMETRICS, BiometricPrompt.BIOMETRIC_ERROR_HW_NOT_PRESENT,
                BiometricPrompt.BIOMETRIC_ERROR_NO_DEVICE_CREDENTIAL ->
                    done(token, UnlockPrompter.Outcome.Failed(UnlockErrors.NO_AUTH))
                // Dismissed by the system (screen off, another app): the request stays open.
                BiometricPrompt.BIOMETRIC_ERROR_CANCELED -> finish()
                else -> if (!retryAfterKeyguard()) done(token, UnlockPrompter.Outcome.Failed(UnlockErrors.DENIED))
            }
        }
    }

    /**
     * Some devices refuse the prompt over the lock screen: dismiss the keyguard (the user unlocks
     * the phone) and show it again, once.
     */
    private fun retryAfterKeyguard(): Boolean {
        val km = getSystemService(KeyguardManager::class.java) ?: return false
        if (keyguardRetried || !km.isKeyguardLocked || SystemClock.elapsedRealtime() - shownAt > KEYGUARD_RETRY_WINDOW_MS) return false
        keyguardRetried = true
        showing = 0
        km.requestDismissKeyguard(this, object : KeyguardManager.KeyguardDismissCallback() {
            override fun onDismissSucceeded() {
                prompter.current.value?.let { show(it) } ?: close()
            }

            override fun onDismissCancelled() {
                prompter.current.value?.let { done(it.token, UnlockPrompter.Outcome.Failed(UnlockErrors.DENIED)) } ?: close()
            }

            override fun onDismissError() {
                prompter.current.value?.let { done(it.token, UnlockPrompter.Outcome.Failed(UnlockErrors.DENIED)) } ?: close()
            }
        })
        return true
    }

    private fun done(token: Long, outcome: UnlockPrompter.Outcome) {
        prompter.finish(token, outcome)
        close()
    }

    private fun close() {
        signal?.cancel()
        signal = null
        showing = 0
        if (!isFinishing) finish()
    }

    companion object {
        private const val TAG = "MD/Unlock"
        /** An error this soon after showing the prompt over the lock screen means it was refused there. */
        private const val KEYGUARD_RETRY_WINDOW_MS = 1_500L

        fun intent(context: Context): Intent = Intent(context, UnlockActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
    }
}
