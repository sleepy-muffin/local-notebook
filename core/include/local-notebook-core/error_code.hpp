#pragma once

namespace lnb::api {
enum class error_code {
  SUCCESS,


  ERR_INVALID_SESSION_ID = 100,
  ERR_SESSION_MEMORY_TRIGGER_HAS_MET = 200,
  ERR_ASSISTANT_MEMORY_TRIGGER_HAS_MET = 201,

  // Another task already owns the session (one turn or fold at a time), or the
  // shared assistant fold is running and would read this session's rows.
  ERR_SESSION_BUSY = 202,
  ERR_ASSISTANT_BUSY = 203,


};


}
