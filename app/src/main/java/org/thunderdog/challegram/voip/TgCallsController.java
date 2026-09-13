/*
 * This file is a part of Telegram X
 * Copyright © 2014 (tgx-android@pm.me)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * File created on 28/03/2023
 */
package org.thunderdog.challegram.voip;

import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import org.drinkless.tdlib.TdApi;
import org.thunderdog.challegram.telegram.Tdlib;
import org.thunderdog.challegram.voip.annotation.AudioState;
import org.thunderdog.challegram.voip.annotation.CallNetworkType;
import org.thunderdog.challegram.voip.annotation.CallRecordingOutputMode;
import org.thunderdog.challegram.voip.annotation.CallRecordingState;
import org.thunderdog.challegram.voip.annotation.VideoState;

@SuppressWarnings("JavaJniMissingFunction")
public class TgCallsController extends VoIPInstance {
  private final String version;
  private long nativePtr;
  private volatile @CallRecordingState int callRecordingState = CallRecordingState.UNSUPPORTED;
  private volatile long callRecordingElapsedSamples;
  private volatile boolean autoCallRecordingEnabled;
  public TgCallsController (@NonNull Tdlib tdlib, @NonNull TdApi.Call call, @NonNull CallConfiguration configuration, @NonNull CallOptions options, @NonNull ConnectionStateListener stateListener, String version) {
    super(tdlib, call, configuration, options, stateListener);
    if (configuration.state.encryptionKey.length != 256)
      throw new IllegalArgumentException(Integer.toString(configuration.state.encryptionKey.length));
    this.version = version;
    this.nativePtr = newInstance(version, configuration, options);
  }

  private long nativePtr () {
    long ptr = nativePtr;
    if (ptr == 0)
      throw new IllegalStateException();
    return ptr;
  }

  private native long newInstance (
    @NonNull String version,
    @NonNull CallConfiguration configuration,
    @NonNull CallOptions options
  );

  private native long preferredConnectionId (long ptr);
  private native @Nullable String lastError (long ptr);
  private native @Nullable String debugLog (long ptr);
  private native void fetchNetworkStats (long ptr, NetworkStats out);

  private native void processIncomingSignalingData (long ptr, byte[] buffer);

  private native void updateNetworkType (long ptr, @CallNetworkType int newType);
  private native void updateMicrophoneDisabled (long ptr, boolean isDisabled);
  private native void updateEchoCancellationStrength (long ptr, int strength);
  private native void updateAudioOutputGainControlEnabled (long ptr, boolean isEnabled);
  private native void destroyInstance (long ptr);
  private native void startCallRecording (long ptr, @CallRecordingOutputMode int outputMode);
  private native void pauseCallRecording (long ptr);
  private native void resumeCallRecording (long ptr);
  private native void stopCallRecording (long ptr);
  private native long callRecordingElapsedSamples (long ptr);

  @Override
  public String getLibraryName () {
    return "tgcalls";
  }

  @Override
  public String getLibraryVersion () {
    return version;
  }

  @Override
  public void initializeAndConnect () {
    // Nothing to do?
  }

  @Override
  protected void handleAudioOutputGainControlEnabled (boolean isEnabled) {
    updateAudioOutputGainControlEnabled(nativePtr(), isEnabled);
  }

  @Override
  protected void handleEchoCancellationStrengthChange (int strength) {
    updateEchoCancellationStrength(nativePtr(), strength);
  }

  @Override
  protected void handleMicDisabled (boolean isDisabled) {
    updateMicrophoneDisabled(nativePtr(), isDisabled);
  }

  @Override
  protected void handleNetworkTypeChange (@CallNetworkType int type) {
    updateNetworkType(nativePtr(), type);
  }

  @Override
  public long getConnectionId () {
    return preferredConnectionId(nativePtr());
  }

  @Nullable
  public String getLastError () {
    return lastError(nativePtr());
  }

  @Override
  public CharSequence collectDebugLog () {
    return debugLog(nativePtr());
  }

  @Override
  public void getNetworkStats (NetworkStats out) {
    fetchNetworkStats(nativePtr(), out);
  }

  @Override
  public @CallRecordingState int getCallRecordingState () {
    return callRecordingState;
  }

  @Override
  public long getCallRecordingElapsedSamples () {
    long ptr = nativePtr;
    return ptr != 0 ? callRecordingElapsedSamples(ptr) : callRecordingElapsedSamples;
  }

  @Override
  public boolean isAutoCallRecordingEnabled () {
    return autoCallRecordingEnabled;
  }

  @Override
  public void startCallRecording (@CallRecordingOutputMode int outputMode) {
    startCallRecording(nativePtr(), outputMode);
  }

  @Override
  public void pauseCallRecording () {
    pauseCallRecording(nativePtr());
  }

  @Override
  public void resumeCallRecording () {
    resumeCallRecording(nativePtr());
  }

  @Override
  public void stopCallRecording () {
    stopCallRecording(nativePtr());
  }

  @Override
  public void performDestroy () {
    if (nativePtr != 0) {
      destroyInstance(nativePtr);
      nativePtr = 0;
    }
  }

  // Called from tgvoip.cpp

  @Keep
  protected final void handleRemoteMediaStateChange (@AudioState int audioState, @VideoState int videoState) {
    connectionStateListener.onRemoteMediaStateChanged(this, audioState, videoState);
  }

  @Keep
  protected final void handleAudioLevelChange (float audioLevel) {
    // TODO
  }
  @Keep
  protected final void handleStop (@NonNull NetworkStats totalStats, @Nullable String debugLog) {
    connectionStateListener.onStopped(this, totalStats, debugLog);
  }

  @Keep
  protected final void handleCallRecordingStateChanged (
    @CallRecordingState int state,
    long elapsedSamples,
    boolean autoRecordingEnabled
  ) {
    this.callRecordingState = state;
    this.callRecordingElapsedSamples = elapsedSamples;
    this.autoCallRecordingEnabled = autoRecordingEnabled;
    connectionStateListener.onCallRecordingStateChanged(
      this,
      state,
      elapsedSamples,
      autoRecordingEnabled
    );
  }

  // Called from TDLib

  @Override
  public void handleIncomingSignalingData (byte[] buffer) {
    processIncomingSignalingData(nativePtr(), buffer);
  }
}
