# Telegram X Recorder v1.0.1

Telegram X Recorder v1.0.1 is a bug-fix release for one-to-one Telegram audio
call recording.

## Fixed

- Fixed `CallRecordingRepository.safeChild()` handling of Android canonical
  path aliases. In particular, `/data/user/0` may canonicalize to `/data/data`.
- The alias mismatch could cause existing valid recordings to be omitted from
  the recordings list. Previously hidden recordings become visible after this
  update.
- Added regression coverage for canonical-path aliases and recording discovery
  across metadata recovery states.

## Unchanged protections and scope

- Canonical containment and symlink protection remain enforced.
- tgcalls and the native recorder are unchanged.
- Recording remains supported for ordinary one-to-one calls only. Group calls
  and Voice Chats are not supported.
