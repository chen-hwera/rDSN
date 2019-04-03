REM SET DSN_TRAVIS=1
SET DSN_AUTO_TEST=1
CALL run.cmd setup-env
CALL run.cmd build Debug .\build build_plugins
CALL run.cmd install Debug .\build
CALL run.cmd start_zk
CALL run.cmd test Debug .\build
