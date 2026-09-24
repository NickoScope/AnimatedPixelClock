#pragma once
// The system corners: two small marks drawn over every page, the same on all.
//
//   top left   A or M. A: the carousel is on and walks the pages by itself;
//              dim while it is held after the knob or the remote turned a page,
//              until it resumes. M: it is off, the pages change by hand. An amber arrow instead
//              while a click has entered a page: the knob and the remote act
//              inside it (stations, lists, volume) rather than turning pages.
//   top right  the Wi-Fi icon, coloured by the signal: green, amber, red; a red
//              cross when not connected. While the remote is being received it
//              is replaced by a blinking red dot, 6 x 6.
//
// **Only for 5 s after the remote was heard** (the owner, 2026-09-24: they got
// in the way of the pictures); otherwise the corners are the page's own.
//
// The owner's design, 2026-09-23. Drawn after the page and before the
// notification banner and the knob's toast, so those stay on top. Each mark
// sits on a black patch one pixel larger than itself, so it reads over any
// picture; that patch is the whole cost to the page under it.
//
// Signal colours: from MetaGeek's "Understanding RSSI" (now at oscium.com,
// read 2026-09-23): -67 dBm "very good" (VoIP, streaming), -70 "okay", -80
// "not good", -90 "unusable". Green at -67 and better, amber to -80, red
// below. One vendor's table: a guide for a colour, nothing decides on it.

void sysCornersDraw();
