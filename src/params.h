#pragma once
// HANDOFF [TODO] params.h - string helpers, Params, suite.cfg parser.
// Source:  engine.h 166-241: trim, lower, pnum, split_ws (167-191), Params (192-202), TestDef (203), Cfg (204-208), parse_cfg_stream (209-241).
// Expose:  all of the above. Params' one-line methods stay inline here; the free functions are defined in params.cpp.
// Needs:   types.h, log.h (parse errors go through emitf), <istream>, <map>, <string>, <vector>.
// Notes:   leave Cfg's apparently unused fields (memPercent, memMB, marginMB, cacheMB) alone: cleanup is a separate commit.
// Delete this block when done.
