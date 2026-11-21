@echo off
rem Testing: every driver (Dark Tooth too) and the secret arenas offered; the signed-in profile's own unlocks are
rem not changed. Put it beside tm2pc.exe.
set TM2PC_UNLOCK_ALL=1
start "" "%~dp0tm2pc.exe" %*
