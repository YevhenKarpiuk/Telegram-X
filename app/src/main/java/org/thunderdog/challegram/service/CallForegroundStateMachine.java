/*
 * This file is a part of Telegram X.
 */
package org.thunderdog.challegram.service;

import androidx.annotation.IntDef;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

public final class CallForegroundStateMachine {
  @Retention(RetentionPolicy.SOURCE)
  @IntDef({State.NONE, State.RINGING, State.ACTIVE_MICROPHONE})
  public @interface State {
    int
      NONE = 0,
      RINGING = 1,
      ACTIVE_MICROPHONE = 2;
  }

  @Retention(RetentionPolicy.SOURCE)
  @IntDef({RequestedType.NONE, RequestedType.SHORT_SERVICE, RequestedType.MICROPHONE})
  public @interface RequestedType {
    int
      NONE = 0,
      SHORT_SERVICE = 1,
      MICROPHONE = 2;
  }

  private CallForegroundStateMachine () { }

  public static @State int decide (
    @State int currentState,
    boolean incoming,
    boolean ringing,
    boolean answerRequested,
    boolean outgoingFromVisibleUi,
    boolean recordAudioGranted,
    boolean finished
  ) {
    if (finished) {
      return State.NONE;
    }
    if (currentState == State.ACTIVE_MICROPHONE) {
      return State.ACTIVE_MICROPHONE;
    }
    if ((answerRequested || outgoingFromVisibleUi) && recordAudioGranted) {
      return State.ACTIVE_MICROPHONE;
    }
    if (incoming && ringing) {
      return State.RINGING;
    }
    return State.NONE;
  }

  public static @RequestedType int requestedTypeForState (@State int state, int sdkInt) {
    return switch (state) {
      case State.RINGING -> sdkInt >= 34 ? RequestedType.SHORT_SERVICE : RequestedType.NONE;
      case State.ACTIVE_MICROPHONE -> sdkInt >= 30 ? RequestedType.MICROPHONE : RequestedType.NONE;
      default -> RequestedType.NONE;
    };
  }

  public static String toString (@State int state) {
    return switch (state) {
      case State.NONE -> "NONE";
      case State.RINGING -> "RINGING";
      case State.ACTIVE_MICROPHONE -> "ACTIVE_MICROPHONE";
      default -> "UNKNOWN(" + state + ")";
    };
  }
}
