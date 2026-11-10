// HANDOFF [TODO] log.cpp - definitions for log.h.
// Source:  engine.h 577-582. Defaults must stay g_console=false, g_tty=true. emit() falls back to stdout when g_emit is empty.
// Notes:   no locking is added: the GUI assigns g_emit before it starts the worker thread (main_gui.cpp start_run).
// Delete this block when done.
