SET DSN_AUTO_TEST=1
run.cmd setup-env && ^
run.cmd build Debug .\build build_plugins && ^
run.cmd install Debug .\build && ^
run.cmd start_zk && ^
run.cmd test Debug .\build
