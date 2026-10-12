# scrcpy-server (guest input agent)

Unmodified `scrcpy-server` 4.1 from <https://github.com/Genymobile/scrcpy>
(Apache-2.0, see `LICENSE`). `tebox-api` pushes it to a running guest over adb
and uses only its control socket (text injection, Android keycodes and
clipboard sync); video and audio stay disabled. The client must announce the
exact version in `VERSION`, so replace both files together.
