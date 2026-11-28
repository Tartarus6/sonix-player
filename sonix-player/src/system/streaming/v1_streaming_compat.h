#ifndef V1_STREAMING_COMPAT_H
#define V1_STREAMING_COMPAT_H

// Compatibility declarations for the ported V1 streaming pages.
//
// These services (SoundCloud, Subsonic, network shares, "keep this track" and
// the podcast heard flag) have no backend in the unified three-device tree.
// Their V1 headers are kept so the pages compile unchanged; the matching
// implementations in v1_streaming_compat.c report "not configured" without
// touching the network, the card or the HiBy settings. Streaming services that
// do exist in this tree (Tidal, Qobuz, radio, podcasts) are not affected.

#include "soundcloud.h"
#include "soundcloudcache.h"
#include "subsonic.h"
#include "subsonicdl.h"
#include "subsonicstream.h"
#include "networkmusic.h"
#include "podcastplayed.h"
#include "streamkeep.h"
#include "streambridge.h"
#include "qobuzgenre_compat.h"
#include "src/system/net/networkshare.h"

#endif /* V1_STREAMING_COMPAT_H */
