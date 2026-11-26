#ifndef FWUPDATE_H
#define FWUPDATE_H

// The "Update firmware" card: from the internet (ota.h) or from the card
// (firmware.h), over a dimmed page.
//
//   choice      Via internet / From SD / Cancel
//   internet    checking -> the newest release, its notes and an Update
//               button, or why there is none -> the download with a progress
//               bar -> recovery, exactly as from the card
//   from SD     the file on the card, or a notice that there is none
//
// The screen stays on while the card is open.

void fwupdate_show(void);

// V1 update notice: start the download of the release ota_release() holds,
// bringing the update card up if it is not already. Used by ota_auto.c; other
// models never call it.
void fwupdate_start_download(void);

#endif /* FWUPDATE_H */
