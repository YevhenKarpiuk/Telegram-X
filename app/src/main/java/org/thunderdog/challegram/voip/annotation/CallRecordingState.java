/*
 * This file is a part of Telegram X
 * Copyright © 2014 (tgx-android@pm.me)
 */
package org.thunderdog.challegram.voip.annotation;

import androidx.annotation.IntDef;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@IntDef({
  CallRecordingState.IDLE,
  CallRecordingState.RECORDING,
  CallRecordingState.PAUSED,
  CallRecordingState.INACTIVE,
  CallRecordingState.FINALIZED,
  CallRecordingState.FAILED,
  CallRecordingState.UNSUPPORTED
})
@Retention(RetentionPolicy.SOURCE)
public @interface CallRecordingState {
  int IDLE = 0;
  int RECORDING = 1;
  int PAUSED = 2;
  int INACTIVE = 3;
  int FINALIZED = 4;
  int FAILED = 5;
  int UNSUPPORTED = 6;
}
