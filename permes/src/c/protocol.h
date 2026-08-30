#pragma once

// AppMessage operation codes shared between the watch (C) and the phone (pkjs).
// Keep in sync with src/pkjs/index.js.

// Watch -> phone requests
#define OP_LIST        1   // request the thread list
#define OP_NEW         2   // create a thread and send its required first TEXT
#define OP_SEND        3   // send TEXT to THREAD_ID (dictated message)
#define OP_OPEN        4   // user opened THREAD_ID; fetch its latest reply

// Phone -> watch responses
#define OP_LIST_BEGIN  10  // COUNT = number of threads that follow
#define OP_THREAD      11  // one thread row: INDEX, THREAD_ID, TITLE, ACTIVE
#define OP_LIST_END    12  // thread list finished
#define OP_NEW_OK      13  // thread created: THREAD_ID, TITLE
#define OP_SEND_OK     14  // message accepted, run started: THREAD_ID
#define OP_REPLY       15  // reply chunk: THREAD_ID, INDEX, COUNT, TEXT
#define OP_STATUS      16  // run status change: THREAD_ID, STATUS
#define OP_ERROR       17  // TEXT = human-readable error
#define OP_TITLE       18  // generated title: THREAD_ID, TITLE

// Run status values (OP_STATUS / ACTIVE flag)
#define STATUS_RUNNING 1   // agent is working ("thinking")
#define STATUS_DONE    2   // final reply delivered
#define STATUS_FAILED  3   // run failed
