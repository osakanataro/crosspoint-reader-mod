#pragma once

// Build marker for this fork ("OST" build).
//
// CROSSPOINT_VERSION identifies the upstream release the tree is based on, and it is load-bearing:
// the OTA check compares against it, the User-Agent carries it, the web API reports it. It is left
// alone. This is a separate marker whose only job is to answer the two questions that were
// previously guesswork -- which of the .bin files on the desk is newest, and which one is on the
// device.
//
// Format: YYYYMMDDNN, the build date plus a counter starting at 01 each day, stamped by
// scripts/ost_version.py. Sorting the number sorts the builds.
#ifndef OST_VERSION
// A build made without the stamping script still has to say something honest.
#define OST_VERSION "unstamped"
#endif

#define OST_VERSION_LABEL "OST " OST_VERSION
