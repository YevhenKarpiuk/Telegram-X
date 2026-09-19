/*
 * This file is a part of Telegram X
 * Copyright © 2014 (tgx-android@pm.me)
 */
package org.thunderdog.challegram.voip.annotation;

import androidx.annotation.IntDef;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@IntDef({
  CallRecordingOutputMode.MIXED_AND_SEPARATE,
  CallRecordingOutputMode.MIXED_ONLY,
  CallRecordingOutputMode.SEPARATE_ONLY
})
@Retention(RetentionPolicy.SOURCE)
public @interface CallRecordingOutputMode {
  int MIXED_AND_SEPARATE = 0;
  int MIXED_ONLY = 1;
  int SEPARATE_ONLY = 2;
}
