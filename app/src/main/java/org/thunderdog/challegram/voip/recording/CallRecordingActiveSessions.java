/*
 * This file is a part of Telegram X
 * Copyright © 2014 (tgx-android@pm.me)
 */
package org.thunderdog.challegram.voip.recording;

import androidx.annotation.NonNull;

import org.thunderdog.challegram.voip.annotation.CallRecordingState;

import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/** Process-local authority for sessions which are active in this process. */
public final class CallRecordingActiveSessions {
  private static final Set<String> ACTIVE = ConcurrentHashMap.newKeySet();

  private CallRecordingActiveSessions () { }

  public static void update (@NonNull String sessionId, @CallRecordingState int state) {
    if (sessionId.isEmpty()) return;
    switch (state) {
      case CallRecordingState.RECORDING:
      case CallRecordingState.PAUSED:
      case CallRecordingState.INACTIVE:
        ACTIVE.add(sessionId);
        break;
      default:
        ACTIVE.remove(sessionId);
        break;
    }
  }

  static boolean isActive (@NonNull String sessionId) {
    return ACTIVE.contains(sessionId);
  }

  static void clearForTests () {
    ACTIVE.clear();
  }
}
