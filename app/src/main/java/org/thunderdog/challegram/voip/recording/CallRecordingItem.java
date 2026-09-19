/*
 * This file is a part of Telegram X
 * Copyright © 2014 (tgx-android@pm.me)
 */
package org.thunderdog.challegram.voip.recording;

import androidx.annotation.Nullable;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class CallRecordingItem {
  public enum Status { COMPLETED, FAILED, INTERRUPTED, INCOMPLETE, IN_PROGRESS }

  public static final class ControlInterval {
    public final long startSample;
    public final long endSample;

    public ControlInterval (long startSample, long endSample) {
      this.startSample = startSample;
      this.endSample = endSample;
    }
  }

  public final String sessionId;
  public final File sessionPath;
  public final int schemaVersion;
  public final long callId;
  public final long userId;
  public final String displayName;
  public final @Nullable Boolean isOutgoing;
  public final long recordingStartTime;
  public final long recordingEndTime;
  public final long callEndTime;
  public final long durationMs;
  public final int timelineSampleRate;
  public final String outputMode;
  public final boolean hasMixed;
  public final boolean hasLocal;
  public final boolean hasRemote;
  public final long mixedSize;
  public final long localSize;
  public final long remoteSize;
  public final long totalSize;
  public final Status status;
  public final List<ControlInterval> pauseIntervals;
  public final List<ControlInterval> stoppedIntervals;

  CallRecordingItem (
    String sessionId,
    File sessionPath,
    int schemaVersion,
    long callId,
    long userId,
    String displayName,
    @Nullable Boolean isOutgoing,
    long recordingStartTime,
    long recordingEndTime,
    long callEndTime,
    long durationMs,
    int timelineSampleRate,
    String outputMode,
    boolean hasMixed,
    boolean hasLocal,
    boolean hasRemote,
    long mixedSize,
    long localSize,
    long remoteSize,
    long totalSize,
    Status status,
    List<ControlInterval> pauseIntervals,
    List<ControlInterval> stoppedIntervals
  ) {
    this.sessionId = sessionId;
    this.sessionPath = sessionPath;
    this.schemaVersion = schemaVersion;
    this.callId = callId;
    this.userId = userId;
    this.displayName = displayName;
    this.isOutgoing = isOutgoing;
    this.recordingStartTime = recordingStartTime;
    this.recordingEndTime = recordingEndTime;
    this.callEndTime = callEndTime;
    this.durationMs = durationMs;
    this.timelineSampleRate = timelineSampleRate;
    this.outputMode = outputMode;
    this.hasMixed = hasMixed;
    this.hasLocal = hasLocal;
    this.hasRemote = hasRemote;
    this.mixedSize = mixedSize;
    this.localSize = localSize;
    this.remoteSize = remoteSize;
    this.totalSize = totalSize;
    this.status = status;
    this.pauseIntervals = Collections.unmodifiableList(new ArrayList<>(pauseIntervals));
    this.stoppedIntervals = Collections.unmodifiableList(new ArrayList<>(stoppedIntervals));
  }

  public boolean isActive () {
    return status == Status.IN_PROGRESS;
  }

  public boolean hasAnyPlayableOutput () {
    return hasMixed || hasLocal || hasRemote;
  }
}
