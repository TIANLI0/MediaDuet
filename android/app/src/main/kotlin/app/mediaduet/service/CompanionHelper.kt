package app.mediaduet.service

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothClass
import android.bluetooth.BluetoothManager
import android.companion.AssociationInfo
import android.companion.AssociationRequest
import android.companion.BluetoothDeviceFilter
import android.companion.CompanionDeviceManager
import android.companion.ObservingDevicePresenceRequest
import android.content.Context
import android.content.IntentSender
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import app.mediaduet.util.MdLog

/**
 * CompanionDeviceManager integration: the user associates the PC through the system chooser,
 * then we observe its presence so the system can wake MediaDuet when the PC comes in range.
 */
class CompanionHelper(private val context: Context) {

    data class Association(val id: Int?, val name: String, val address: String?)

    /** A computer this phone is bonded with over Bluetooth. */
    data class BondedPc(val name: String, val address: String)

    private val cdm: CompanionDeviceManager? =
        if (context.packageManager.hasSystemFeature(PackageManager.FEATURE_COMPANION_DEVICE_SETUP)) {
            context.getSystemService(CompanionDeviceManager::class.java)
        } else null

    val isSupported: Boolean get() = cdm != null

    /** Presence observation needs API 31+. */
    val canObservePresence: Boolean get() = cdm != null && Build.VERSION.SDK_INT >= 31

    fun associations(): List<Association> {
        val m = cdm ?: return emptyList()
        return try {
            if (Build.VERSION.SDK_INT >= 33) {
                m.myAssociations.map { a ->
                    Association(a.id, a.displayName?.toString() ?: a.deviceMacAddress?.toString() ?: "#${a.id}", a.deviceMacAddress?.toString())
                }
            } else {
                @Suppress("DEPRECATION")
                m.associations.map { Association(null, it, it) }
            }
        } catch (e: Exception) {
            MdLog.w(TAG, "cannot list associations", e)
            emptyList()
        }
    }

    /**
     * Bonded Bluetooth computers that are not associated yet. The system chooser only scans for
     * discoverable devices; a bonded PC is listed only when asked for by its address ([associate]).
     */
    @SuppressLint("MissingPermission") // checked below
    fun bondedComputers(): List<BondedPc> {
        if (cdm == null) return emptyList()
        if (Build.VERSION.SDK_INT >= 31 &&
            context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
        ) return emptyList()
        val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter ?: return emptyList()
        val associated = associations().mapNotNull { it.address?.uppercase() }.toSet()
        return try {
            adapter.bondedDevices.orEmpty()
                .filter { it.bluetoothClass?.majorDeviceClass == BluetoothClass.Device.Major.COMPUTER }
                .filter { it.address.uppercase() !in associated }
                .map { BondedPc(it.name?.takeIf { n -> n.isNotBlank() } ?: it.address, it.address) }
                .sortedBy { it.name.lowercase() }
        } catch (e: SecurityException) {
            MdLog.w(TAG, "cannot list bonded devices: ${e.message}")
            emptyList()
        }
    }

    interface Callback {
        fun onChooser(sender: IntentSender)
        fun onFailure(error: String?)
    }

    /**
     * Starts the system association flow for a Bluetooth device (the PC): the bonded device at
     * [address], or (null) a scan for discoverable devices.
     */
    fun associate(callback: Callback, address: String? = null) {
        val m = cdm ?: return callback.onFailure("unsupported")
        val filter = BluetoothDeviceFilter.Builder().apply { if (address != null) setAddress(address) }.build()
        val request = AssociationRequest.Builder()
            .addDeviceFilter(filter)
            .setSingleDevice(address != null)
            .build()
        val cb = object : CompanionDeviceManager.Callback() {
            @Deprecated("API 33 uses onAssociationPending")
            override fun onDeviceFound(intentSender: IntentSender) = callback.onChooser(intentSender)
            override fun onAssociationPending(intentSender: IntentSender) = callback.onChooser(intentSender)
            override fun onAssociationCreated(associationInfo: AssociationInfo) {
                MdLog.i(TAG, "association created")
                observeAll()
            }

            override fun onFailure(error: CharSequence?) = callback.onFailure(error?.toString())
        }
        try {
            if (Build.VERSION.SDK_INT >= 33) {
                m.associate(request, context.mainExecutor, cb)
            } else {
                @Suppress("DEPRECATION")
                m.associate(request, cb, Handler(Looper.getMainLooper()))
            }
        } catch (e: Exception) {
            callback.onFailure(e.message)
        }
    }

    /** (Re)starts presence observation for every association. */
    fun observeAll() {
        val m = cdm ?: return
        if (Build.VERSION.SDK_INT < 31) return
        for (a in associations()) {
            try {
                if (Build.VERSION.SDK_INT >= 36 && a.id != null) {
                    m.startObservingDevicePresence(ObservingDevicePresenceRequest.Builder().setAssociationId(a.id).build())
                } else if (a.address != null) {
                    @Suppress("DEPRECATION")
                    m.startObservingDevicePresence(a.address)
                }
            } catch (e: Exception) {
                MdLog.w(TAG, "cannot observe presence of ${a.name}: ${e.message}")
            }
        }
    }

    fun disassociate(a: Association) {
        val m = cdm ?: return
        try {
            if (Build.VERSION.SDK_INT >= 33 && a.id != null) {
                m.disassociate(a.id)
            } else if (a.address != null) {
                @Suppress("DEPRECATION")
                m.disassociate(a.address)
            }
        } catch (e: Exception) {
            MdLog.w(TAG, "disassociate failed", e)
        }
    }

    private companion object {
        const val TAG = "MD/CDM"
    }
}
