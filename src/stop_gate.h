// What the GUI's Stop button and its window close do while a run is going. Pure logic with no window code, so
// tools/unit_tests.cpp can test the rules on every platform; src/main_gui.cpp only carries out the answers.
// Used from the UI thread only (no locking).
//
// Rules (audit item B2):
//  - The UI thread never waits for the worker. Stop and close only ask the run to stop; the window is closed when
//    the worker's "done" message arrives (on_run_done answers CloseNow), so it keeps painting and answering.
//  - A stop can take a long time (a multi-GB lock, one big OS call), so after `delay` seconds the Stop button turns
//    into "Force quit", and a second close click does the same: both end the process at once, like the CLI's
//    second Ctrl-C. Until then a second click does nothing, so a double click cannot kill the program by
//    accident. Nothing is lost by a force quit: every error line is flushed to the log as it is reported.
#pragma once

namespace ddr5 {

class StopGate {
  public:
    enum class Act {
        None,        // nothing to do
        RequestStop, // ask the run to stop (the caller sets Run::stop and shows "Stopping...")
        ForceExit,   // end the process now (the caller closes the log first, best effort)
        CloseNow     // no run is going: destroy the window
    };

    explicit StopGate(double delaySeconds = 3.0) : delay_(delaySeconds) {}

    void on_run_start() {
        running_ = true;
        stopping_ = false;
        closing_ = false;
        offered_ = false;
        stopAt_ = 0;
    }

    // The Stop button was clicked while a run is going.
    Act on_stop_click(double now) {
        if (!running_)
            return Act::None;
        if (!stopping_)
            return begin_stop(now);
        return offered_ ? Act::ForceExit : Act::None;
    }

    // The user closed the window (X, Alt+F4, system menu).
    Act on_close(double now) {
        if (!running_)
            return Act::CloseNow;
        closing_ = true;
        if (!stopping_)
            return begin_stop(now);
        return offered_ ? Act::ForceExit : Act::None;
    }

    // Called from the UI timer. True exactly once per run: the moment the Stop button should become "Force quit".
    bool tick(double now) {
        if (!running_ || !stopping_ || offered_ || now - stopAt_ < delay_)
            return false;
        offered_ = true;
        return true;
    }

    // The worker's done message arrived. CloseNow if the user closed the window meanwhile.
    Act on_run_done() {
        running_ = false;
        stopping_ = false;
        offered_ = false;
        return closing_ ? Act::CloseNow : Act::None;
    }

    bool running() const { return running_; }
    bool stopping() const { return stopping_; }
    bool closing() const { return closing_; }
    bool force_offered() const { return offered_; }

  private:
    Act begin_stop(double now) {
        stopping_ = true;
        stopAt_ = now;
        return Act::RequestStop;
    }

    double delay_;
    double stopAt_ = 0;
    bool running_ = false, stopping_ = false, closing_ = false, offered_ = false;
};

} // namespace ddr5
