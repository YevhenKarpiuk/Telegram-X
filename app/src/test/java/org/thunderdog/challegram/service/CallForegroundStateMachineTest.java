/*
 * This file is a part of Telegram X Recorder.
 */
package org.thunderdog.challegram.service;

import org.junit.Test;

import static org.junit.Assert.assertEquals;

public class CallForegroundStateMachineTest {
  @Test
  public void incomingRingingUsesRingingState () {
    int state = decide(CallForegroundStateMachine.State.NONE,
      true, true, false, false, true, false);
    assertEquals(CallForegroundStateMachine.State.RINGING, state);
    assertEquals(CallForegroundStateMachine.RequestedType.SHORT_SERVICE,
      CallForegroundStateMachine.requestedTypeForState(state, 34));
    assertEquals(CallForegroundStateMachine.RequestedType.NONE,
      CallForegroundStateMachine.requestedTypeForState(state, 33));
  }

  @Test
  public void answeredIncomingWithPermissionPromotesToMicrophone () {
    int state = decide(CallForegroundStateMachine.State.RINGING,
      true, true, true, false, true, false);
    assertEquals(CallForegroundStateMachine.State.ACTIVE_MICROPHONE, state);
    assertEquals(CallForegroundStateMachine.RequestedType.MICROPHONE,
      CallForegroundStateMachine.requestedTypeForState(state, 35));
    assertEquals(CallForegroundStateMachine.RequestedType.NONE,
      CallForegroundStateMachine.requestedTypeForState(state, 29));
  }

  @Test
  public void rejectedIncomingNeverPromotesToMicrophone () {
    assertEquals(CallForegroundStateMachine.State.NONE,
      decide(CallForegroundStateMachine.State.RINGING, true, false, false, false, true, true));
  }

  @Test
  public void missedIncomingNeverPromotesToMicrophone () {
    assertEquals(CallForegroundStateMachine.State.NONE,
      decide(CallForegroundStateMachine.State.RINGING, true, false, false, false, true, true));
  }

  @Test
  public void outgoingFromVisibleUiPromotesToMicrophone () {
    assertEquals(CallForegroundStateMachine.State.ACTIVE_MICROPHONE,
      decide(CallForegroundStateMachine.State.NONE, false, false, false, true, true, false));
  }

  @Test
  public void deniedRecordAudioNeverPromotesToMicrophone () {
    assertEquals(CallForegroundStateMachine.State.RINGING,
      decide(CallForegroundStateMachine.State.RINGING, true, true, true, false, false, false));
  }

  @Test
  public void activeCallNeverDowngradesToRinging () {
    assertEquals(CallForegroundStateMachine.State.ACTIVE_MICROPHONE,
      decide(CallForegroundStateMachine.State.ACTIVE_MICROPHONE, true, true, false, false, true, false));
  }

  private static int decide (
    int currentState,
    boolean incoming,
    boolean ringing,
    boolean answerRequested,
    boolean outgoingFromVisibleUi,
    boolean recordAudioGranted,
    boolean finished
  ) {
    return CallForegroundStateMachine.decide(
      currentState,
      incoming,
      ringing,
      answerRequested,
      outgoingFromVisibleUi,
      recordAudioGranted,
      finished
    );
  }
}
