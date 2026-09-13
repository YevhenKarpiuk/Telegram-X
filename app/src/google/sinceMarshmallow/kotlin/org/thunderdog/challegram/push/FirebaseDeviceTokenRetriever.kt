package org.thunderdog.challegram.push

import android.content.Context
import com.google.android.gms.tasks.OnSuccessListener
import com.google.firebase.installations.FirebaseInstallations
import com.google.firebase.messaging.FirebaseMessaging
import org.drinkless.tdlib.TdApi.DeviceTokenFirebaseCloudMessaging
import org.thunderdog.challegram.service.DefaultFirebaseTokenRetriever
import tgx.bridge.PushManagerBridge
import tgx.bridge.TokenRetrieverListener

class FirebaseDeviceTokenRetriever : DefaultFirebaseTokenRetriever() {
  override fun fetchDeviceToken(context: Context, listener: TokenRetrieverListener) {
    try {
      PushManagerBridge.log("TGX-Push: FCM token acquisition requested")
      FirebaseMessaging.getInstance().register().addOnSuccessListener(OnSuccessListener { _ ->
        PushManagerBridge.log("TGX-Push: FirebaseMessaging registered, obtaining installation ID")
        FirebaseInstallations.getInstance().id
          .addOnSuccessListener { installationId ->
            PushManagerBridge.log("TGX-Push: FCM token acquired: yes, length: %d", installationId.length)
            listener.onTokenRetrievalSuccess(DeviceTokenFirebaseCloudMessaging(installationId, true))
          }
          .addOnFailureListener { e: Exception? ->
            val errorName = extractFirebaseErrorName(e!!)
            PushManagerBridge.error(
              "TGX-Push: Firebase installation ID fetch failed ($errorName)",
              e
            )
            listener.onTokenRetrievalError(errorName, e)
          }
      }).addOnFailureListener { e: Exception? ->
        val errorName = extractFirebaseErrorName(e!!)
        PushManagerBridge.error(
          "TGX-Push: FirebaseMessaging registration failed ($errorName)",
          e
        )
        listener.onTokenRetrievalError(errorName, e)
      }
    } catch (e: Throwable) {
      PushManagerBridge.error("TGX-Push: FCM token acquisition failed with error", e)
      listener.onTokenRetrievalError("FIREBASE_REQUEST_ERROR", e)
    }
  }
}
